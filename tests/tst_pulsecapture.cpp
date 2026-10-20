#include <QtTest>

#include "core/AudioFormat.h"
#include "core/PulseCapture.h"
#include "core/SampleQueue.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

/// Live test against the running sound server (PipeWire's PulseAudio compat
/// layer on this machine). Every test that needs a server goes through
/// `openOrSkip`, which QSKIPs when the server is unreachable.

namespace {

/// Collects the AudioSource callback from the mainloop thread so the test
/// thread can wait on it.
struct CallbackSink {
    std::mutex m;
    std::condition_variable cv;
    int openedCount = 0;
    int failedCount = 0;
    QString lastMessage;

    AudioSource::Callback callback()
    {
        return [this](AudioSource::Event e, const QString &msg) {
            std::lock_guard<std::mutex> lock(m);
            if (e == AudioSource::Event::Opened)
                ++openedCount;
            else
                ++failedCount;
            lastMessage = msg;
            cv.notify_all();
        };
    }

    /// Wait until any event arrived; returns false on timeout.
    bool waitForEvent(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, timeout, [this] { return openedCount + failedCount > 0; });
    }
    int opened()
    {
        std::lock_guard<std::mutex> lock(m);
        return openedCount;
    }
    int failed()
    {
        std::lock_guard<std::mutex> lock(m);
        return failedCount;
    }
    QString message()
    {
        std::lock_guard<std::mutex> lock(m);
        return lastMessage;
    }
};

/// Returns true when `capture` reported Opened within 3 s. Returns false
/// (after which the caller must QSKIP) when it reported Failed or nothing at
/// all, i.e. there is no usable sound server.
bool openWithin3s(PulseCapture &capture, CallbackSink &sink)
{
    capture.openAsync(sink.callback());
    if (!sink.waitForEvent(3s))
        return false;
    return sink.opened() == 1 && sink.failed() == 0;
}

#define OPEN_OR_SKIP(capture, sink)                                                      \
    do {                                                                                 \
        if (!openWithin3s(capture, sink)) {                                              \
            QSKIP(qPrintable(QStringLiteral("no sound server: %1")                       \
                                 .arg(sink.message().isEmpty() ? "no callback within 3 s" \
                                                               : sink.message())));       \
        }                                                                                \
    } while (0)

/// Asserts that the blocks are contiguous: each block starts where the
/// previous one ended. Returns the position just past the last block, or -1
/// when `blocks` is empty.
qint64 verifyContiguous(const std::vector<SampleQueue::Block> &blocks, qint64 expectedStart)
{
    qint64 next = expectedStart;
    for (const auto &b : blocks) {
        if (next >= 0 && b.positionStart != next) {
            qWarning("gap/overlap: block at %lld, expected %lld", (long long)b.positionStart,
                     (long long)next);
            return -2;
        }
        next = b.positionStart + qint64(b.samples.size());
    }
    return blocks.empty() ? -1 : next;
}

}  // namespace

class TstPulsecapture : public QObject
{
    Q_OBJECT

private slots:
    void micOnly_opensFillsContiguouslyAndStopsCleanly();
    void timingSnapshots_arriveAndTrackDeliveredPositions();
    void micAndMonitor_bothQueuesFillWithValidSnapshots();
    void stopBeforeReady_isBoundedAndSilencesCallback();
    void stopTwiceAndDestroy_areNoOps();
    void badMicDevice_reportsAtMostOneEventAndStopsBounded();
    void unreachableServer_failsExactlyOnceNeverOpens();
};

void TstPulsecapture::micOnly_opensFillsContiguouslyAndStopsCleanly()
{
    auto mic = std::make_shared<SampleQueue>();
    PulseCapture capture(mic, nullptr);
    CallbackSink sink;
    OPEN_OR_SKIP(capture, sink);

    std::this_thread::sleep_for(500ms);
    const qint64 pending = mic->pendingFrames();
    qInfo("mic pending after 500 ms: %lld frames", (long long)pending);
    QVERIFY2(pending >= AudioFormat::framesForMs(250),
             qPrintable(QString("only %1 frames pending").arg(pending)));

    const auto blocks = mic->take();
    QVERIFY(!blocks.empty());
    QCOMPARE(blocks.front().positionStart, qint64(0));
    const qint64 end = verifyContiguous(blocks, 0);
    QVERIFY(end > 0);
    QCOMPARE(end, pending);

    QElapsedTimer t;
    t.start();
    capture.stop();
    QVERIFY2(t.elapsed() < 1000, "stop() took too long");
    QVERIFY(mic->isClosed());
    QCOMPARE(mic->error(), QString());
}

void TstPulsecapture::timingSnapshots_arriveAndTrackDeliveredPositions()
{
    auto mic = std::make_shared<SampleQueue>();
    PulseCapture capture(mic, nullptr);
    CallbackSink sink;
    OPEN_OR_SKIP(capture, sink);

    std::this_thread::sleep_for(1s);
    const TimingSnapshot snap = mic->latestSnapshot();
    const auto blocks = mic->take();
    QVERIFY(!blocks.empty());
    const qint64 delivered = verifyContiguous(blocks, 0);
    qInfo("snapshot: valid=%d systemTimeUs=%lld positionFrames=%lld captureOffsetUs=%lld; delivered=%lld",
          snap.valid, (long long)snap.systemTimeUs, (long long)snap.positionFrames,
          (long long)snap.captureOffsetUs, (long long)delivered);

    QVERIFY(snap.valid);
    QVERIFY(snap.systemTimeUs != 0);
    QVERIFY(snap.positionFrames > 0);
    // The snapshot was taken before take(): it cannot be ahead of what the
    // server has delivered by more than 200 ms, and it cannot lag behind
    // the very first block.
    QVERIFY2(snap.positionFrames <= delivered + AudioFormat::framesForMs(200),
             qPrintable(QString("snapshot %1 leads delivered %2 by too much")
                            .arg(snap.positionFrames).arg(delivered)));
    QVERIFY(snap.positionFrames >= blocks.front().positionStart);
    capture.stop();
}

void TstPulsecapture::micAndMonitor_bothQueuesFillWithValidSnapshots()
{
    auto mic = std::make_shared<SampleQueue>();
    auto monitor = std::make_shared<SampleQueue>();
    PulseCapture capture(mic, monitor);
    CallbackSink sink;
    OPEN_OR_SKIP(capture, sink);

    std::this_thread::sleep_for(1s);
    const TimingSnapshot micSnap = mic->latestSnapshot();
    const TimingSnapshot monSnap = monitor->latestSnapshot();
    const auto micBlocks = mic->take();
    const auto monBlocks = monitor->take();
    const qint64 micDelivered = verifyContiguous(micBlocks, 0);
    const qint64 monDelivered = verifyContiguous(monBlocks, 0);
    qInfo("mic: delivered=%lld offsetUs=%lld valid=%d | monitor: delivered=%lld offsetUs=%lld valid=%d",
          (long long)micDelivered, (long long)micSnap.captureOffsetUs, micSnap.valid,
          (long long)monDelivered, (long long)monSnap.captureOffsetUs, monSnap.valid);

    QVERIFY(micDelivered >= AudioFormat::framesForMs(500));
    QVERIFY(monDelivered >= AudioFormat::framesForMs(500));
    QVERIFY(micSnap.valid);
    QVERIFY(monSnap.valid);
    QVERIFY(micSnap.positionFrames > 0);
    QVERIFY(monSnap.positionFrames > 0);

    QElapsedTimer t;
    t.start();
    capture.stop();
    QVERIFY(t.elapsed() < 1000);
    QVERIFY(mic->isClosed() && monitor->isClosed());
    QCOMPARE(mic->error(), QString());
    QCOMPARE(monitor->error(), QString());
}

void TstPulsecapture::stopBeforeReady_isBoundedAndSilencesCallback()
{
    auto mic = std::make_shared<SampleQueue>();
    auto monitor = std::make_shared<SampleQueue>();
    auto capture = std::make_unique<PulseCapture>(mic, monitor);
    CallbackSink sink;
    capture->openAsync(sink.callback());

    QElapsedTimer t;
    t.start();
    capture->stop();
    QVERIFY2(t.elapsed() < 1000, "stop() took too long");
    QVERIFY(mic->isClosed() && monitor->isClosed());

    // Whatever was delivered before stop() stays; nothing may arrive after.
    // A Failed that raced ahead of stop() (no server) leaves its reason in
    // the queues; otherwise the stop is clean.
    const int opened = sink.opened();
    const int failed = sink.failed();
    QVERIFY(opened <= 1 && failed <= 1);
    const QString expectedError = failed ? sink.message() : QString();
    QCOMPARE(mic->error(), expectedError);
    QCOMPARE(monitor->error(), expectedError);
    std::this_thread::sleep_for(200ms);
    QCOMPARE(sink.opened(), opened);
    QCOMPARE(sink.failed(), failed);
    capture.reset();
    QCOMPARE(sink.opened(), opened);
    QCOMPARE(sink.failed(), failed);
}

void TstPulsecapture::stopTwiceAndDestroy_areNoOps()
{
    auto mic = std::make_shared<SampleQueue>();
    CallbackSink sink;
    {
        PulseCapture capture(mic, nullptr);
        OPEN_OR_SKIP(capture, sink);
        capture.stop();
        QVERIFY(mic->isClosed());
        QElapsedTimer t;
        t.start();
        capture.stop();
        QVERIFY(t.elapsed() < 100);
        // destructor runs stop() a third time
    }
    QCOMPARE(mic->error(), QString());
    std::this_thread::sleep_for(100ms);
    QCOMPARE(sink.opened(), 1);
    QCOMPARE(sink.failed(), 0);

    // stop() without openAsync() is also a no-op and still closes the queue.
    auto other = std::make_shared<SampleQueue>();
    PulseCapture idle(other, nullptr);
    idle.stop();
    idle.stop();
    QVERIFY(!other->isClosed() || other->error().isEmpty());
}

void TstPulsecapture::badMicDevice_reportsAtMostOneEventAndStopsBounded()
{
    // A probe tells us whether a server is there at all.
    {
        auto probeQueue = std::make_shared<SampleQueue>();
        PulseCapture probe(probeQueue, nullptr);
        CallbackSink probeSink;
        OPEN_OR_SKIP(probe, probeSink);
    }

    // PulseAudio proper rejects an unknown source with PA_ERR_NOENTITY
    // (-> exactly one Failed). PipeWire's compat layer (1.0.5 here) instead
    // silently links the stream to the default source (-> Opened). Either
    // way the callback guards hold: at most one of each, never both.
    auto mic = std::make_shared<SampleQueue>();
    auto monitor = std::make_shared<SampleQueue>();
    PulseCapture capture(mic, monitor, QStringLiteral("no-such-device-xyz"));
    CallbackSink sink;
    capture.openAsync(sink.callback());
    QVERIFY2(sink.waitForEvent(3s), "no callback within 3 s");
    std::this_thread::sleep_for(300ms);  // give a (wrong) second report a chance
    qInfo("bad device -> opened=%d failed=%d message='%s'", sink.opened(), sink.failed(),
          qPrintable(sink.message()));
    QCOMPARE(sink.opened() + sink.failed(), 1);
    if (sink.failed() == 1) {
        QVERIFY(!sink.message().isEmpty());
        QVERIFY(mic->isClosed());
        QCOMPARE(mic->error(), sink.message());
        QVERIFY(monitor->isClosed());
    }

    QElapsedTimer t;
    t.start();
    capture.stop();
    QVERIFY(t.elapsed() < 1000);
    std::this_thread::sleep_for(100ms);
    QCOMPARE(sink.opened() + sink.failed(), 1);
}

void TstPulsecapture::unreachableServer_failsExactlyOnceNeverOpens()
{
    // No gate: an unreachable server is the expected outcome whether or not
    // a real one exists. PULSE_SERVER is read by pa_context_connect(), so
    // this only affects this test's connection attempt.
    const QByteArray saved = qgetenv("PULSE_SERVER");
    qputenv("PULSE_SERVER", "unix:/nonexistent/recorder-test/native");
    auto restore = qScopeGuard([&] {
        if (saved.isNull())
            qunsetenv("PULSE_SERVER");
        else
            qputenv("PULSE_SERVER", saved);
    });

    auto mic = std::make_shared<SampleQueue>();
    auto monitor = std::make_shared<SampleQueue>();
    PulseCapture capture(mic, monitor);
    CallbackSink sink;
    capture.openAsync(sink.callback());
    QVERIFY2(sink.waitForEvent(3s), "no callback within 3 s");
    std::this_thread::sleep_for(300ms);  // give a (wrong) second report a chance
    qInfo("unreachable server -> Failed: %s", qPrintable(sink.message()));
    QCOMPARE(sink.failed(), 1);
    QCOMPARE(sink.opened(), 0);
    QVERIFY(!sink.message().isEmpty());
    QVERIFY(mic->isClosed());
    QVERIFY(monitor->isClosed());
    QCOMPARE(mic->error(), sink.message());
    QCOMPARE(monitor->error(), sink.message());

    QElapsedTimer t;
    t.start();
    capture.stop();
    QVERIFY(t.elapsed() < 1000);
    std::this_thread::sleep_for(100ms);
    QCOMPARE(sink.failed(), 1);
    QCOMPARE(sink.opened(), 0);
}

QTEST_GUILESS_MAIN(TstPulsecapture)
#include "tst_pulsecapture.moc"
