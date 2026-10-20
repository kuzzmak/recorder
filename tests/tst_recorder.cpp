#include <QtTest>

#include "core/AudioFormat.h"
#include "core/OutputPath.h"
#include "core/Recorder.h"
#include "core/WavWriter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int kChunk = AudioFormat::kFramesPerChunk;

/// Wait for `pred` WITHOUT running the event loop (so queued signals stay
/// queued); false after `ms`.
template <typename Pred>
bool spinUntil(Pred pred, int ms = 2000)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (Clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// A gate the test opens by hand; remembers when it was opened.
struct Latch {
    std::mutex m;
    std::condition_variable cv;
    bool released = false;
    Clock::time_point releasedAt;

    void release()
    {
        std::lock_guard<std::mutex> lock(m);
        released = true;
        releasedAt = Clock::now();
        cv.notify_all();
    }
    void wait()
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this] { return released; });
    }
};

/// Observable state of the fake source; outlives the source itself, which
/// the Recorder owns and destroys when the session completes.
struct SourceStats {
    std::atomic<int> created{0};
    std::atomic<int> stopCalls{0};
    std::atomic<bool> hasMic{false};
    std::atomic<bool> hasMonitor{false};
    Clock::time_point firstStop;
};

/// Capture backend double. openAsync() stores the callback; the test fires
/// it (fireOpened / fireFailed) or lets the producer thread report an
/// overflow, exactly as PulseCapture does.
class FakeSource : public AudioSource
{
public:
    FakeSource(const Recorder::Queues &queues, std::shared_ptr<SourceStats> stats)
        : m_queues(queues), m_stats(std::move(stats))
    {
        ++m_stats->created;
        m_stats->hasMic = queues.mic != nullptr;
        m_stats->hasMonitor = queues.monitor != nullptr;
    }
    ~FakeSource() override { shutdownQueues(); }   // implicit stop: not counted

    void openAsync(Callback cb) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cb = std::move(cb);
    }

    void fireOpened() { fire(Event::Opened, QString()); }
    void fireFailed(const QString &msg) { fire(Event::Failed, msg); }
    /// Fire from a foreign thread, like the pulse mainloop does.
    void fireOpenedFromThread()
    {
        std::thread([this] { fireOpened(); }).join();
    }

    void startProducing(int framesPerTick, int intervalMs)
    {
        stopProducing();
        m_running = true;
        m_producer = std::thread([this, framesPerTick, intervalMs] {
            qint64 pos = 0;
            qint64 t = 1000000;
            while (m_running) {
                const bool ok = m_queues.mic->pushSilence(std::size_t(framesPerTick), pos);
                if (m_queues.monitor)
                    m_queues.monitor->pushSilence(std::size_t(framesPerTick), pos);
                pos += framesPerTick;
                t += qint64(intervalMs) * 1000;
                m_queues.mic->pushSnapshot({t, pos, 5000, true});
                if (m_queues.monitor)
                    m_queues.monitor->pushSnapshot({t, pos, 5000, true});
                if (!ok) {
                    // The backend's job: an overflow-closed queue is fatal.
                    fire(Event::Failed, m_queues.mic->error());
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            }
        });
    }
    void stopProducing()
    {
        m_running = false;
        if (m_producer.joinable())
            m_producer.join();
    }

    void stop() override
    {
        if (m_stats->stopCalls++ == 0)
            m_stats->firstStop = Clock::now();
        shutdownQueues();
    }

private:
    void shutdownQueues()
    {
        stopProducing();
        if (m_queues.mic)
            m_queues.mic->close();
        if (m_queues.monitor)
            m_queues.monitor->close();
    }
    void fire(Event e, const QString &msg)
    {
        Callback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cb = m_cb;
        }
        if (cb)
            cb(e, msg);
    }

    Recorder::Queues m_queues;
    std::shared_ptr<SourceStats> m_stats;
    std::mutex m_mutex;
    Callback m_cb;
    std::atomic<bool> m_running{false};
    std::thread m_producer;
};

/// Knobs and counters of the fake writer, shared with the test.
struct WriterStats {
    bool failOnOpen = false;
    int failOnWrite = 0;          ///< n-th write fails (0 => never)
    bool failOnFinalize = false;
    std::shared_ptr<Latch> blockWrites;   ///< write() blocks until released

    std::atomic<int> opens{0};
    std::atomic<int> blockedWrites{0};   ///< writes currently parked on the latch
    std::atomic<int> writes{0};
    std::atomic<qint64> framesWritten{0};
    std::atomic<bool> finalized{false};
    std::atomic<QThread *> workerThread{nullptr};   ///< side channel: the EncoderThread
};

class FakeWriter : public AudioWriter
{
public:
    explicit FakeWriter(std::shared_ptr<WriterStats> s) : m_s(std::move(s)) {}
    QString extension() const override { return QStringLiteral("fake"); }
    bool open(const QString &path, QString *err) override
    {
        ++m_s->opens;
        if (m_s->failOnOpen) {
            *err = QStringLiteral("disk full");
            return false;
        }
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            *err = m_file.errorString();
            return false;
        }
        return true;
    }
    bool write(const int16_t *pcm, std::size_t frames, QString *err) override
    {
        if (m_s->blockWrites) {
            ++m_s->blockedWrites;
            m_s->blockWrites->wait();
        }
        const int n = ++m_s->writes;
        if (m_s->failOnWrite && n == m_s->failOnWrite) {
            *err = QStringLiteral("EIO");
            return false;
        }
        m_file.write(reinterpret_cast<const char *>(pcm), qint64(frames * sizeof(int16_t)));
        m_s->framesWritten += qint64(frames);
        return true;
    }
    bool finalize(QString *err) override
    {
        if (m_s->finalized)
            return true;
        m_s->finalized = true;
        m_file.close();
        if (m_s->failOnFinalize) {
            *err = QStringLiteral("flush failed");
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<WriterStats> m_s;
    QFile m_file;
};

/// One Recorder wired to the doubles, with spies on every signal.
struct Harness {
    QTemporaryDir dir;
    std::shared_ptr<SourceStats> src = std::make_shared<SourceStats>();
    std::shared_ptr<WriterStats> wr = std::make_shared<WriterStats>();
    FakeSource *source = nullptr;   ///< valid until the session completes
    Recorder recorder;
    QSignalSpy stateChanged{&recorder, &Recorder::stateChanged};
    QSignalSpy started{&recorder, &Recorder::started};
    QSignalSpy stopped{&recorder, &Recorder::stopped};
    QSignalSpy errors{&recorder, &Recorder::error};

    Harness()
        : recorder(
              [this](const Recorder::Queues &q, const Recorder::SessionSpec &) {
                  auto s = std::make_unique<FakeSource>(q, src);
                  source = s.get();
                  return std::unique_ptr<AudioSource>(std::move(s));
              },
              [this](const Recorder::SessionSpec &) {
                  if (QThread::currentThread() != QCoreApplication::instance()->thread())
                      wr->workerThread = QThread::currentThread();
                  return std::unique_ptr<AudioWriter>(std::make_unique<FakeWriter>(wr));
              })
    {
        recorder.setStartDeadlineMs(2000);
        recorder.setFinishWatchdogMs(2000);
        recorder.setEncoderConfig(10, 2000);
    }

    Recorder::SessionSpec spec(bool systemAudio = false) const
    {
        Recorder::SessionSpec s;
        s.outputDir = dir.path();
        s.format = QStringLiteral("fake");
        s.captureSystemAudio = systemAudio;
        return s;
    }

    QString errorText(int i = 0) const { return errors.at(i).at(0).toString(); }
    QString stoppedPath(int i = 0) const { return stopped.at(i).at(0).toString(); }
    qint64 stoppedDuration(int i = 0) const { return stopped.at(i).at(1).toLongLong(); }
    Recorder::Outcome stoppedOutcome(int i = 0) const
    {
        return stopped.at(i).at(2).value<Recorder::Outcome>();
    }
};
}  // namespace

class TstRecorder : public QObject
{
    Q_OBJECT

private slots:
    void start_entersStartingAndCreatesSourceWithQueues();
    void stopBeforeOpened_stopsSourceAndFailsOnce();
    void stopAfterOpenedBeforeCaptureStarted_swallowsLateStart();
    void openNeverCompletes_deadlineFailsSession();
    void sourceFailed_stopsWithMessage();
    void happyPath_recordsAndSaves();
    void writerOpenFailure_failsAndRemovesPart();
    void writeFailure_stopsSourceAndSavesTruncated();
    void finalizeFailure_unfinalizedWithPartPath();
    void renameFailure_renameFailedWithPartPath();
    void noProgress_watchdogFailsSession();
    void queueOverflowWithBlockedWriter_stopsSourceBeforeWriterUnblocks();
    void restartFromStoppedSlot_succeedsWithNewPath();
    void startWhileCleanupPending_refused();
    void secondSession_hasFreshBookkeeping();
    void shutdown_idleAndWhileStarting();
    void shutdownWithBlockedWriter_detachesWorkerWhichFinishesAlone();
    void outcomeText_formatsEveryOutcome();
    void finishWatchdog_warnsAndRearmsWhileWorkerIsStuck();
};

void TstRecorder::start_entersStartingAndCreatesSourceWithQueues()
{
    Harness h;
    QVERIFY(h.recorder.start(h.spec(false)));
    QCOMPARE(h.recorder.state(), Recorder::State::Starting);
    QCOMPARE(h.stateChanged.count(), 1);
    QCOMPARE(h.src->created.load(), 1);
    QVERIFY(h.src->hasMic);
    QVERIFY(!h.src->hasMonitor);
    QVERIFY(h.errors.isEmpty());

    Harness g;
    QVERIFY(g.recorder.start(g.spec(true)));
    QVERIFY(g.src->hasMic);
    QVERIFY(g.src->hasMonitor);
}

void TstRecorder::stopBeforeOpened_stopsSourceAndFailsOnce()
{
    Harness h;
    QVERIFY(h.recorder.start(h.spec()));
    h.recorder.stop();
    QTRY_COMPARE(h.stopped.count(), 1);
    QCOMPARE(h.src->stopCalls.load(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Failed);
    QVERIFY(h.stoppedPath().isEmpty());
    QCOMPARE(h.started.count(), 0);
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
    QVERIFY(!h.recorder.lastResult().fileFinalized);
    QVERIFY(QDir(h.dir.path()).entryList(QDir::Files).isEmpty());   // no .part left

    // Nothing arrives late.
    QVERIFY(!h.stopped.wait(50));
    QCOMPARE(h.stopped.count(), 1);
}

void TstRecorder::stopAfterOpenedBeforeCaptureStarted_swallowsLateStart()
{
    Harness h;
    QVERIFY(h.recorder.start(h.spec()));
    h.source->fireOpenedFromThread();   // relayed through the event loop …
    h.recorder.stop();                  // … but we stop before it is processed
    QTRY_COMPARE(h.stopped.count(), 1);
    QCOMPARE(h.started.count(), 0);
    QCOMPARE(h.src->stopCalls.load(), 1);
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
    QVERIFY(!h.stopped.wait(50));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.started.count(), 0);

    // And the variant where the Opened event was processed (worker created)
    // but captureStarted has not been relayed yet.
    Harness g;
    QVERIFY(g.recorder.start(g.spec()));
    g.source->fireOpened();
    QCoreApplication::processEvents();   // onSourceEvent runs: worker starts
    QVERIFY(spinUntil([&] { return g.wr->opens.load() == 1; }));   // captureStarted is queued now
    g.recorder.stop();
    QTRY_COMPARE(g.stopped.count(), 1);
    QCOMPARE(g.started.count(), 0);
    QCOMPARE(g.recorder.state(), Recorder::State::Idle);
    QVERIFY(!g.stopped.wait(50));
    QCOMPARE(g.stopped.count(), 1);
    QCOMPARE(g.started.count(), 0);
    QVERIFY(QDir(g.dir.path()).entryList(QDir::Files).isEmpty());   // zero frames => Failed, no .part
}

void TstRecorder::openNeverCompletes_deadlineFailsSession()
{
    Harness h;
    h.recorder.setStartDeadlineMs(100);
    QElapsedTimer t;
    t.start();
    QVERIFY(h.recorder.start(h.spec()));
    QVERIFY(h.stopped.wait(2000));
    QVERIFY(t.elapsed() >= 90);
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Failed);
    QCOMPARE(h.errors.count(), 1);
    QCOMPARE(h.errorText(), QStringLiteral("Sound server did not respond"));
    QCOMPARE(h.src->stopCalls.load(), 1);
    QCOMPARE(h.wr->opens.load(), 0);   // no worker ever existed
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
}

void TstRecorder::sourceFailed_stopsWithMessage()
{
    Harness h;
    QVERIFY(h.recorder.start(h.spec()));
    std::thread([&] { h.source->fireFailed(QStringLiteral("Connection refused")); }).join();
    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Failed);
    QCOMPARE(h.errors.count(), 1);
    QCOMPARE(h.errorText(), QStringLiteral("Connection refused"));
    QCOMPARE(h.src->stopCalls.load(), 1);
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
}

void TstRecorder::happyPath_recordsAndSaves()
{
    Harness h;
    QSignalSpy ticks(&h.recorder, &Recorder::tick);
    QSignalSpy levels(&h.recorder, &Recorder::levelChanged);
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();

    QVERIFY(h.started.wait(2000));
    QCOMPARE(h.started.count(), 1);
    const QString finalPath = h.started.at(0).at(0).toString();
    QVERIFY(finalPath.startsWith(h.dir.path()));
    QVERIFY(finalPath.endsWith(QStringLiteral(".fake")));
    QVERIFY(!finalPath.contains(QStringLiteral("_sys")));
    QCOMPARE(h.recorder.state(), Recorder::State::Recording);
    QCOMPARE(h.recorder.currentPath(), finalPath);
    QVERIFY(QFileInfo::exists(OutputPath::tempFor(finalPath)));

    QTRY_VERIFY(h.wr->framesWritten.load() >= 5 * kChunk);
    QVERIFY(h.recorder.elapsedMs() > 0);
    QTRY_VERIFY(levels.count() >= 1);

    h.recorder.stop();
    QCOMPARE(h.recorder.state(), Recorder::State::Stopping);
    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Saved);
    QCOMPARE(h.stoppedPath(), finalPath);
    QVERIFY(h.stoppedDuration() > 0);
    QCOMPARE(h.errors.count(), 0);
    QCOMPARE(h.src->stopCalls.load(), 1);
    QVERIFY(h.wr->finalized);
    QVERIFY(QFileInfo::exists(finalPath));
    QVERIFY(!QFileInfo::exists(OutputPath::tempFor(finalPath)));
    QVERIFY(h.recorder.lastResult().framesWritten > 0);
    QVERIFY(h.recorder.lastResult().fileFinalized);
    QCOMPARE(h.recorder.lastResult().framesWritten, h.wr->framesWritten.load());
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
    QCOMPARE(h.recorder.elapsedMs(), h.stoppedDuration());
}

void TstRecorder::writerOpenFailure_failsAndRemovesPart()
{
    Harness h;
    h.wr->failOnOpen = true;
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();

    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.started.count(), 0);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Failed);
    QVERIFY(h.stoppedPath().isEmpty());
    QCOMPARE(h.errors.count(), 1);
    QVERIFY(h.errorText().startsWith(QStringLiteral("Cannot create file")));
    QCOMPARE(h.src->stopCalls.load(), 1);
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
    QVERIFY(QDir(h.dir.path()).entryList(QDir::Files).isEmpty());
}

void TstRecorder::writeFailure_stopsSourceAndSavesTruncated()
{
    Harness h;
    h.wr->failOnWrite = 3;
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();
    QVERIFY(h.started.wait(2000));
    const QString finalPath = h.started.at(0).at(0).toString();

    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.src->stopCalls.load(), 1);
    QCOMPARE(h.errors.count(), 1);
    QVERIFY(h.errorText().startsWith(QStringLiteral("Write failed")));
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::SavedTruncated);
    QCOMPARE(h.stoppedPath(), finalPath);
    QVERIFY(QFileInfo::exists(finalPath));
    QVERIFY(!QFileInfo::exists(OutputPath::tempFor(finalPath)));
    QCOMPARE(h.recorder.lastResult().framesWritten, qint64(2 * kChunk));
    QVERIFY(h.recorder.lastResult().error.startsWith(QStringLiteral("Write failed")));
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
}

void TstRecorder::finalizeFailure_unfinalizedWithPartPath()
{
    Harness h;
    h.wr->failOnFinalize = true;
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();
    QVERIFY(h.started.wait(2000));
    const QString finalPath = h.started.at(0).at(0).toString();
    QTRY_VERIFY(h.wr->framesWritten.load() >= 2 * kChunk);

    h.recorder.stop();
    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Unfinalized);
    QCOMPARE(h.stoppedPath(), OutputPath::tempFor(finalPath));
    QVERIFY(QFileInfo::exists(OutputPath::tempFor(finalPath)));
    QVERIFY(!QFileInfo::exists(finalPath));
    QCOMPARE(h.errors.count(), 1);
    QVERIFY(h.errorText().contains(QStringLiteral("flush failed")));
    QVERIFY(!h.recorder.lastResult().fileFinalized);
    QVERIFY(h.recorder.lastResult().framesWritten > 0);
}

void TstRecorder::renameFailure_renameFailedWithPartPath()
{
    Harness h;
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();
    QVERIFY(h.started.wait(2000));
    const QString finalPath = h.started.at(0).at(0).toString();
    QTRY_VERIFY(h.wr->framesWritten.load() >= 2 * kChunk);
    QVERIFY(QDir().mkdir(finalPath));   // a directory in the way: rename cannot succeed

    h.recorder.stop();
    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::RenameFailed);
    QCOMPARE(h.stoppedPath(), OutputPath::tempFor(finalPath));
    QVERIFY(QFileInfo(OutputPath::tempFor(finalPath)).isFile());
    QCOMPARE(h.errors.count(), 0);   // the outcome says it; no separate error
    QVERIFY(h.recorder.lastResult().fileFinalized);
}

void TstRecorder::noProgress_watchdogFailsSession()
{
    Harness h;
    h.recorder.setEncoderConfig(10, 200);
    QVERIFY(h.recorder.start(h.spec()));
    h.source->fireOpenedFromThread();   // opened, but nothing is ever pushed
    QVERIFY(h.started.wait(2000));
    QCOMPARE(h.recorder.state(), Recorder::State::Recording);

    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Failed);   // zero frames
    QVERIFY(h.stoppedPath().isEmpty());
    QCOMPARE(h.errors.count(), 1);
    QVERIFY(h.errorText().contains(QStringLiteral("No audio")));
    QCOMPARE(h.src->stopCalls.load(), 1);
    QVERIFY(QDir(h.dir.path()).entryList(QDir::Files).isEmpty());
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
}

void TstRecorder::queueOverflowWithBlockedWriter_stopsSourceBeforeWriterUnblocks()
{
    Harness h;
    h.recorder.setQueueCapacityFrames(5 * kChunk);
    auto latch = std::make_shared<Latch>();
    h.wr->blockWrites = latch;
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 2);
    h.source->fireOpenedFromThread();
    QVERIFY(h.started.wait(2000));

    // The producer overflows the 5-chunk queue while the writer is stuck;
    // the backend reports it and the Recorder must stop the source at once,
    // without waiting for the worker.
    QTRY_COMPARE(h.src->stopCalls.load(), 1);
    QCOMPARE(h.recorder.state(), Recorder::State::Stopping);
    QCOMPARE(h.stopped.count(), 0);
    latch->release();

    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QVERIFY(h.src->firstStop < latch->releasedAt);
    const Recorder::Outcome o = h.stoppedOutcome();
    QVERIFY(o == Recorder::Outcome::SavedTruncated || o == Recorder::Outcome::Unfinalized);
    QCOMPARE(h.errors.count(), 1);
    QVERIFY(h.errorText().contains(QStringLiteral("overflow")));
    QVERIFY(h.recorder.lastResult().framesWritten > 0);
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
}

void TstRecorder::restartFromStoppedSlot_succeedsWithNewPath()
{
    Harness h;
    bool restarted = false;
    connect(&h.recorder, &Recorder::stopped, &h.recorder, [&] {
        if (!restarted) {
            restarted = true;
            QVERIFY(h.recorder.start(h.spec()));
            h.source->startProducing(kChunk, 5);
            h.source->fireOpened();
        }
    });
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();
    QVERIFY(h.started.wait(2000));
    const QString first = h.started.at(0).at(0).toString();
    QTRY_VERIFY(h.wr->framesWritten.load() >= 2 * kChunk);
    h.recorder.stop();

    QVERIFY(h.started.wait(2000));   // the second session
    QCOMPARE(h.started.count(), 2);
    QCOMPARE(h.stopped.count(), 1);
    const QString second = h.started.at(1).at(0).toString();
    QVERIFY(second != first);
    QCOMPARE(h.src->created.load(), 2);
    QCOMPARE(h.recorder.state(), Recorder::State::Recording);

    h.recorder.stop();
    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 2);
    QCOMPARE(h.stoppedOutcome(1), Recorder::Outcome::Saved);
    QCOMPARE(h.stoppedPath(1), second);
    QVERIFY(QFileInfo::exists(first));
    QVERIFY(QFileInfo::exists(second));
    QCOMPARE(h.errors.count(), 0);
}

void TstRecorder::startWhileCleanupPending_refused()
{
    Harness h;
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();
    QVERIFY(h.started.wait(2000));
    QVERIFY(!h.recorder.start(h.spec()));   // Recording
    QCOMPARE(h.errors.count(), 1);

    h.recorder.stop();
    QCOMPARE(h.recorder.state(), Recorder::State::Stopping);
    QVERIFY(!h.recorder.start(h.spec()));   // worker still finishing
    QCOMPARE(h.errors.count(), 2);
    QVERIFY(!h.errorText(1).isEmpty());
    QCOMPARE(h.src->created.load(), 1);

    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Saved);
    QVERIFY(h.recorder.start(h.spec()));    // clean again
    QCOMPARE(h.src->created.load(), 2);
}

void TstRecorder::secondSession_hasFreshBookkeeping()
{
    Harness h;
    // First session fails (source error) …
    QVERIFY(h.recorder.start(h.spec()));
    h.source->fireFailed(QStringLiteral("Connection refused"));
    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.errors.count(), 1);
    QCOMPARE(h.recorder.lastResult().framesWritten, qint64(0));

    // … the second one is untouched by it: no stale error, no stale
    // "completed" flag, its own result.
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();
    QVERIFY(h.started.wait(2000));
    QTRY_VERIFY(h.wr->framesWritten.load() >= 2 * kChunk);
    h.recorder.stop();
    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 2);
    QCOMPARE(h.stoppedOutcome(1), Recorder::Outcome::Saved);
    QCOMPARE(h.errors.count(), 1);
    QVERIFY(h.recorder.lastResult().error.isEmpty());
    QVERIFY(h.recorder.lastResult().framesWritten > 0);
    QVERIFY(h.stoppedDuration(1) > 0);
    QVERIFY(!h.stopped.wait(50));
    QCOMPARE(h.stopped.count(), 2);
}

void TstRecorder::shutdown_idleAndWhileStarting()
{
    {
        Harness h;
        QVERIFY(h.recorder.shutdown(100));
        QCOMPARE(h.stateChanged.count(), 0);
        QCOMPARE(h.stopped.count(), 0);
        QCOMPARE(h.errors.count(), 0);
        QVERIFY(!h.recorder.start(h.spec()));   // refused after shutdown
        QCOMPARE(h.errors.count(), 1);
    }
    {
        Harness h;
        bool restartResult = true;
        connect(&h.recorder, &Recorder::stopped, &h.recorder,
                [&] { restartResult = h.recorder.start(h.spec()); });
        QVERIFY(h.recorder.start(h.spec()));
        QVERIFY(h.recorder.shutdown(100));   // open callback never came
        QCOMPARE(h.src->stopCalls.load(), 1);
        QCOMPARE(h.stopped.count(), 1);
        QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Failed);
        QVERIFY(!restartResult);
        QCOMPARE(h.src->created.load(), 1);
        QCOMPARE(h.recorder.state(), Recorder::State::Idle);
    }
}

void TstRecorder::shutdownWithBlockedWriter_detachesWorkerWhichFinishesAlone()
{
    QTemporaryDir outerDir;   // outlives the Harness so the .part can be inspected
    auto latch = std::make_shared<Latch>();
    std::shared_ptr<WriterStats> wr;
    std::shared_ptr<SourceStats> src;
    QString finalPath;
    {
        Harness h;
        wr = h.wr;
        src = h.src;
        h.wr->blockWrites = latch;
        Recorder::SessionSpec spec = h.spec();
        spec.outputDir = outerDir.path();
        QVERIFY(h.recorder.start(spec));
        h.source->startProducing(kChunk, 5);
        h.source->fireOpenedFromThread();
        QVERIFY(h.started.wait(2000));
        finalPath = h.started.at(0).at(0).toString();
        QTRY_VERIFY(h.wr->blockedWrites.load() >= 1);

        QElapsedTimer t;
        t.start();
        QVERIFY(!h.recorder.shutdown(100));
        QVERIFY(t.elapsed() >= 90);
        QCOMPARE(h.src->stopCalls.load(), 1);
        QCOMPARE(h.stopped.count(), 0);
        QCOMPARE(h.recorder.state(), Recorder::State::Stopping);
        QVERIFY(!h.recorder.start(h.spec()));
        QVERIFY(wr->workerThread.load() != nullptr);
    }   // the Recorder dies with the worker still parked in write()

    QVERIFY(!wr->finalized);
    latch->release();
    QThread *worker = wr->workerThread;
    QVERIFY(worker->wait(2000));
    QVERIFY(wr->finalized);
    QVERIFY(wr->framesWritten.load() > 0);
    QVERIFY(QFileInfo::exists(OutputPath::tempFor(finalPath)));   // nobody renamed it
    delete worker;   // the test owns the detached thread now
}

void TstRecorder::outcomeText_formatsEveryOutcome()
{
    using O = Recorder::Outcome;
    const QString p = QStringLiteral("/tmp/x/name.ogg");
    const QString part = QStringLiteral("/tmp/x/name.ogg.part");
    QCOMPARE(Recorder::outcomeText(O::Saved, p, (12 * 60 + 34) * 1000, QString()),
             QStringLiteral("Saved 12:34 \u2192 name.ogg"));
    QCOMPARE(Recorder::outcomeText(O::Saved, p, (3 * 3600 + 5 * 60 + 7) * 1000, QString()),
             QStringLiteral("Saved 3:05:07 \u2192 name.ogg"));
    QCOMPARE(Recorder::outcomeText(O::Saved, p, 999, QString()),
             QStringLiteral("Saved 00:00 \u2192 name.ogg"));
    QCOMPARE(Recorder::outcomeText(O::SavedTruncated, p, (10 * 60 + 2) * 1000, QStringLiteral("Write failed: EIO")),
             QStringLiteral("Recording stopped early (Write failed: EIO). Saved 10:02 \u2192 name.ogg"));
    QCOMPARE(Recorder::outcomeText(O::RenameFailed, part, 5000, QString()),
             QStringLiteral("Saved but could not rename; file is name.ogg.part"));
    QCOMPARE(Recorder::outcomeText(O::Unfinalized, part, 5000, QStringLiteral("Could not finalize: x")),
             QStringLiteral("Could not finalize; partial file name.ogg.part"));
    QCOMPARE(Recorder::outcomeText(O::Failed, QString(), 0, QStringLiteral("Sound server did not respond")),
             QStringLiteral("Could not record: Sound server did not respond"));
}

void TstRecorder::finishWatchdog_warnsAndRearmsWhileWorkerIsStuck()
{
    Harness h;
    h.recorder.setFinishWatchdogMs(100);
    auto latch = std::make_shared<Latch>();
    h.wr->blockWrites = latch;
    QVERIFY(h.recorder.start(h.spec()));
    h.source->startProducing(kChunk, 5);
    h.source->fireOpenedFromThread();
    QVERIFY(h.started.wait(2000));
    const QString finalPath = h.started.at(0).at(0).toString();
    QTRY_VERIFY(h.wr->blockedWrites.load() >= 1);

    h.recorder.stop();
    QVERIFY(h.errors.wait(2000));
    QCOMPARE(h.errors.count(), 1);
    QVERIFY(h.errorText().contains(QStringLiteral("taking longer")));
    QCOMPARE(h.recorder.state(), Recorder::State::Stopping);
    QCOMPARE(h.stopped.count(), 0);
    QVERIFY(h.errors.wait(2000));   // re-armed: warns again
    QCOMPARE(h.errors.count(), 2);
    QCOMPARE(h.stopped.count(), 0);

    latch->release();
    QVERIFY(h.stopped.wait(2000));
    QCOMPARE(h.stopped.count(), 1);
    QCOMPARE(h.stoppedOutcome(), Recorder::Outcome::Saved);
    QCOMPARE(h.stoppedPath(), finalPath);
    QVERIFY(!h.errors.wait(150));   // the watchdog is off once complete
    QCOMPARE(h.errors.count(), 2);
    QCOMPARE(h.recorder.state(), Recorder::State::Idle);
}

QTEST_GUILESS_MAIN(TstRecorder)
#include "tst_recorder.moc"
