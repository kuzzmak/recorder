#include <QtTest>

#include "core/SampleQueue.h"

#include <thread>

class TstSamplequeue : public QObject
{
    Q_OBJECT

private slots:
    void push_thenTake_returnsBlockWithPosition();
    void pushSilence_yieldsZeroBlock();
    void push_overflow_refusedAndClosedWithError();
    void close_cleanHasNoError_firstReasonWins();
    void snapshot_latestIsReturned();
    void waitForChange_timesOutWhenIdle_wakesOnPushAndClose();
};

void TstSamplequeue::push_thenTake_returnsBlockWithPosition()
{
    SampleQueue q;
    const int16_t pcm[4] = {1, 2, 3, 4};
    QVERIFY(q.push(pcm, 4, 100));
    QCOMPARE(q.pendingFrames(), 4);

    const auto blocks = q.take();
    QCOMPARE(blocks.size(), std::size_t(1));
    QCOMPARE(blocks[0].positionStart, 100);
    QCOMPARE(blocks[0].samples, (std::vector<int16_t>{1, 2, 3, 4}));
    QCOMPARE(q.pendingFrames(), 0);
}

void TstSamplequeue::pushSilence_yieldsZeroBlock()
{
    SampleQueue q;
    QVERIFY(q.pushSilence(3, 7));
    const auto blocks = q.take();
    QCOMPARE(blocks.size(), std::size_t(1));
    QCOMPARE(blocks[0].positionStart, 7);
    QCOMPARE(blocks[0].samples, (std::vector<int16_t>{0, 0, 0}));
}

void TstSamplequeue::push_overflow_refusedAndClosedWithError()
{
    SampleQueue q(10);
    QVERIFY(q.pushSilence(6, 0));
    QVERIFY(q.pushSilence(4, 6));            // exactly at capacity is fine
    QVERIFY(!q.pushSilence(1, 10));          // one more would exceed it
    QVERIFY(q.isClosed());
    QCOMPARE(q.error(), QString::fromLatin1(SampleQueue::kOverflowError));
    QCOMPARE(q.pendingFrames(), 10);         // what was accepted stays readable
    QVERIFY(!q.take().empty());
    QVERIFY(!q.pushSilence(1, 11));          // closed: ignored even with room
    QCOMPARE(q.pendingFrames(), 0);
}

void TstSamplequeue::close_cleanHasNoError_firstReasonWins()
{
    SampleQueue q;
    QVERIFY(!q.isClosed());
    q.close();
    QVERIFY(q.isClosed());
    QVERIFY(q.error().isEmpty());
    q.close("later error");   // idempotent: the first (clean) close wins
    QVERIFY(q.error().isEmpty());

    SampleQueue e;
    e.close("Connection terminated");
    e.close("Context failed");
    QCOMPARE(e.error(), QStringLiteral("Connection terminated"));
}

void TstSamplequeue::snapshot_latestIsReturned()
{
    SampleQueue q;
    QVERIFY(!q.latestSnapshot().valid);
    q.pushSnapshot({1000, 48, 20, true});
    q.pushSnapshot({2000, 96, 21, true});
    const TimingSnapshot s = q.latestSnapshot();
    QVERIFY(s.valid);
    QCOMPARE(s.systemTimeUs, 2000);
    QCOMPARE(s.positionFrames, 96);
    QCOMPARE(s.captureOffsetUs, 21);
}

void TstSamplequeue::waitForChange_timesOutWhenIdle_wakesOnPushAndClose()
{
    SampleQueue q;
    QElapsedTimer t;
    t.start();
    QVERIFY(!q.waitForChange(50));
    QVERIFY(t.elapsed() >= 45);

    std::thread producer([&q] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.pushSilence(1, 0);
    });
    t.restart();
    QVERIFY(q.waitForChange(5000));
    QVERIFY(t.elapsed() < 4000);
    producer.join();
    QVERIFY(q.waitForChange(0));   // data still pending: returns at once
    q.take();

    std::thread closer([&q] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.close();
    });
    QVERIFY(q.waitForChange(5000));
    closer.join();
    QVERIFY(q.waitForChange(0));   // closed: never blocks again
}

QTEST_GUILESS_MAIN(TstSamplequeue)
#include "tst_samplequeue.moc"
