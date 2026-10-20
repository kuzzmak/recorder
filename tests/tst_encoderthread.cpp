#include <QtTest>

#include "core/AudioFormat.h"
#include "core/EncoderThread.h"
#include "core/WavWriter.h"

#include <atomic>
#include <memory>
#include <vector>

namespace {
constexpr int kChunk = AudioFormat::kFramesPerChunk;

/// Push `chunks` 960-frame blocks of silence into `q` at consecutive positions.
void pushChunks(SampleQueue &q, int chunks, qint64 startPos = 0)
{
    for (int i = 0; i < chunks; ++i)
        QVERIFY(q.pushSilence(kChunk, startPos + qint64(i) * kChunk));
}

/// Collects the worker's signals on the main thread (queued), because a
/// QSignalSpy would record them on the emitting thread.
struct Events : QObject {
    int started = 0;
    int failed = 0;
    int levels = 0;
    quint64 startedId = 0;
    quint64 failedId = 0;
    quint64 levelId = 0;
    QString failedMessage;
    int lastPeak = -1;

    explicit Events(EncoderThread *t)
    {
        connect(t, &EncoderThread::captureStarted, this, [this](quint64 id) {
            ++started;
            startedId = id;
        }, Qt::QueuedConnection);
        connect(t, &EncoderThread::sessionFailed, this, [this](quint64 id, const QString &m) {
            ++failed;
            failedId = id;
            failedMessage = m;
        }, Qt::QueuedConnection);
        connect(t, &EncoderThread::levelChanged, this, [this](quint64 id, int peak) {
            ++levels;
            levelId = id;
            lastPeak = peak;
        }, Qt::QueuedConnection);
    }
};

/// Let queued signals arrive for `ms` without any expectation.
void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// Knobs and counters of the writer double; the test keeps them alive
/// after the worker has destroyed the writer itself.
struct WriterStats {
    int failOnWrite = 0;      ///< n-th write fails (0 => never)
    bool failOnOpen = false;
    bool failOnFinalize = false;
    std::atomic<int> writes{0};
    std::atomic<bool> finalized{false};
};

/// Minimal writer double: counts calls and fails on demand.
struct CountingWriter : AudioWriter {
    std::shared_ptr<WriterStats> s;
    explicit CountingWriter(std::shared_ptr<WriterStats> stats = std::make_shared<WriterStats>())
        : s(std::move(stats))
    {
    }

    QString extension() const override { return QStringLiteral("fake"); }
    bool open(const QString &path, QString *err) override
    {
        if (s->failOnOpen) {
            *err = QStringLiteral("disk full");
            return false;
        }
        QFile f(path);
        return f.open(QIODevice::WriteOnly);
    }
    bool write(const int16_t *, std::size_t, QString *err) override
    {
        const int n = ++s->writes;
        if (s->failOnWrite && n == s->failOnWrite) {
            *err = QStringLiteral("EIO");
            return false;
        }
        return true;
    }
    bool finalize(QString *err) override
    {
        s->finalized = true;
        if (s->failOnFinalize) {
            *err = QStringLiteral("flush failed");
            return false;
        }
        return true;
    }
};
}  // namespace

class TstEncoderthread : public QObject
{
    Q_OBJECT

private slots:
    void micOnly_writesPushedFramesAndDrainsOnStop();
    void captureStarted_emittedOnceWithSessionId();
    void writeFailure_failsOnceAndKeepsRunningUntilStop();
    void noProgress_failsWithNoAudioMessage();
    void errorClosedQueue_failsWithThatReason();
    void openFailure_failsAndFinishesOnlyAfterStop();
    void levelChanged_everyNthChunkWithPeak();
};

void TstEncoderthread::micOnly_writesPushedFramesAndDrainsOnStop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString temp = dir.filePath(QStringLiteral("out.wav.part"));

    auto mic = std::make_shared<SampleQueue>();
    EncoderThread::Config cfg;
    cfg.tempPath = temp;
    cfg.waitMs = 10;
    cfg.noProgressMs = 5000;
    EncoderThread thread(1, mic, nullptr, [] { return std::make_unique<WavWriter>(); }, cfg);
    thread.start();

    pushChunks(*mic, 10);
    mic->close();
    thread.requestStop();   // whatever the loop did not pick up is drained
    QVERIFY(thread.wait(2000));

    const CaptureResult r = thread.result();
    QCOMPARE(r.framesWritten, qint64(10 * kChunk));
    QVERIFY(r.fileFinalized);
    QVERIFY(r.error.isEmpty());

    QFile f(temp);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.size(), qint64(44 + 10 * kChunk * 2));
}

void TstEncoderthread::captureStarted_emittedOnceWithSessionId()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto mic = std::make_shared<SampleQueue>();
    EncoderThread::Config cfg;
    cfg.tempPath = dir.filePath(QStringLiteral("out.wav.part"));
    cfg.waitMs = 10;
    EncoderThread thread(42, mic, nullptr, [] { return std::make_unique<WavWriter>(); }, cfg);
    Events ev(&thread);
    thread.start();

    QTRY_COMPARE(ev.started, 1);
    QCOMPARE(ev.startedId, quint64(42));

    mic->close();
    thread.requestStop();
    QVERIFY(thread.wait(2000));
    pump(20);
    QCOMPARE(ev.started, 1);
}

void TstEncoderthread::writeFailure_failsOnceAndKeepsRunningUntilStop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto mic = std::make_shared<SampleQueue>();
    auto w = std::make_shared<WriterStats>();
    w->failOnWrite = 2;
    EncoderThread::Config cfg;
    cfg.tempPath = dir.filePath(QStringLiteral("out.fake.part"));
    cfg.waitMs = 10;
    EncoderThread thread(1, mic, nullptr,
                         [w] { return std::unique_ptr<AudioWriter>(new CountingWriter(w)); }, cfg);
    Events ev(&thread);
    thread.start();

    pushChunks(*mic, 3);
    QTRY_COMPARE(ev.failed, 1);
    QCOMPARE(ev.failedId, quint64(1));
    QVERIFY(ev.failedMessage.startsWith(QStringLiteral("Write failed")));

    // Still running: more pushes are drained and discarded, no second failure.
    pushChunks(*mic, 3, 3 * kChunk);
    QTRY_COMPARE(mic->pendingFrames(), qint64(0));
    pump(100);
    QVERIFY(thread.isRunning());
    QCOMPARE(ev.failed, 1);
    QCOMPARE(w->writes.load(), 2);

    mic->close();
    thread.requestStop();
    QVERIFY(thread.wait(2000));
    const CaptureResult r = thread.result();
    QVERIFY(r.error.contains(QStringLiteral("EIO")));
    QCOMPARE(r.framesWritten, qint64(kChunk));
    QVERIFY(r.fileFinalized);   // what was written is still closed properly
    QVERIFY(w->finalized);
}

void TstEncoderthread::noProgress_failsWithNoAudioMessage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto mic = std::make_shared<SampleQueue>();
    EncoderThread::Config cfg;
    cfg.tempPath = dir.filePath(QStringLiteral("out.fake.part"));
    cfg.waitMs = 10;
    cfg.noProgressMs = 200;
    EncoderThread thread(1, mic, nullptr, [] { return std::make_unique<CountingWriter>(); }, cfg);
    Events ev(&thread);
    QElapsedTimer t;
    t.start();
    thread.start();

    QTRY_COMPARE(ev.failed, 1);
    QVERIFY(t.elapsed() >= 150);
    QVERIFY(ev.failedMessage.startsWith(QStringLiteral("No audio")));
    pump(50);
    QVERIFY(thread.isRunning());   // the owner ends the session, not the worker

    mic->close();
    thread.requestStop();
    QVERIFY(thread.wait(2000));
    pump(20);
    QCOMPARE(ev.failed, 1);
    QVERIFY(thread.result().error.startsWith(QStringLiteral("No audio")));
}

void TstEncoderthread::errorClosedQueue_failsWithThatReason()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto mic = std::make_shared<SampleQueue>();
    EncoderThread::Config cfg;
    cfg.tempPath = dir.filePath(QStringLiteral("out.fake.part"));
    cfg.waitMs = 10;
    EncoderThread thread(1, mic, nullptr, [] { return std::make_unique<CountingWriter>(); }, cfg);
    Events ev(&thread);
    thread.start();

    pushChunks(*mic, 2);
    mic->close(QStringLiteral("Connection terminated"));
    QTRY_COMPARE(ev.failed, 1);
    QCOMPARE(ev.failedMessage, QStringLiteral("Connection terminated"));
    pump(50);
    QVERIFY(thread.isRunning());

    thread.requestStop();
    QVERIFY(thread.wait(2000));
    const CaptureResult r = thread.result();
    QCOMPARE(r.error, QStringLiteral("Connection terminated"));
    QCOMPARE(r.framesWritten, qint64(2 * kChunk));   // what arrived before is kept
    QVERIFY(r.fileFinalized);
}

void TstEncoderthread::openFailure_failsAndFinishesOnlyAfterStop()
{
    auto mic = std::make_shared<SampleQueue>();
    auto w = std::make_shared<WriterStats>();
    w->failOnOpen = true;
    EncoderThread::Config cfg;
    cfg.tempPath = QStringLiteral("/nonexistent/dir/out.fake.part");
    cfg.waitMs = 10;
    EncoderThread thread(1, mic, nullptr,
                         [w] { return std::unique_ptr<AudioWriter>(new CountingWriter(w)); }, cfg);
    Events ev(&thread);
    thread.start();

    QTRY_COMPARE(ev.failed, 1);
    QVERIFY(ev.failedMessage.startsWith(QStringLiteral("Cannot create file")));
    QVERIFY(ev.failedMessage.contains(QStringLiteral("disk full")));
    pump(50);
    QVERIFY(thread.isRunning());
    QCOMPARE(ev.started, 0);

    pushChunks(*mic, 2);   // discarded, never written
    mic->close();
    thread.requestStop();
    QVERIFY(thread.wait(2000));
    const CaptureResult r = thread.result();
    QVERIFY(!r.fileFinalized);
    QCOMPARE(r.framesWritten, qint64(0));
    QCOMPARE(w->writes.load(), 0);
    QVERIFY(!w->finalized);
}

void TstEncoderthread::levelChanged_everyNthChunkWithPeak()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto mic = std::make_shared<SampleQueue>();
    EncoderThread::Config cfg;
    cfg.tempPath = dir.filePath(QStringLiteral("out.fake.part"));
    cfg.waitMs = 10;
    cfg.levelEveryChunks = 5;
    EncoderThread thread(7, mic, nullptr, [] { return std::make_unique<CountingWriter>(); }, cfg);
    Events ev(&thread);

    std::vector<int16_t> pcm(kChunk, 1000);
    for (int i = 0; i < 10; ++i)
        QVERIFY(mic->push(pcm.data(), pcm.size(), qint64(i) * kChunk));
    mic->close();
    thread.start();
    thread.requestStop();
    QVERIFY(thread.wait(2000));
    QTRY_COMPARE(ev.levels, 2);   // chunks 5 and 10 (queued signals)
    QCOMPARE(ev.levelId, quint64(7));
    QCOMPARE(ev.lastPeak, 1000);
}

QTEST_GUILESS_MAIN(TstEncoderthread)
#include "tst_encoderthread.moc"
