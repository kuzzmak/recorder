#include <QtTest>

#include "core/StreamClock.h"

#include <cmath>

namespace {
/// Snapshot of a synthetic stream running at `rateHz` whose sample 0 was
/// captured at system time `baseUs`, measured `tUs` after that.
TimingSnapshot synth(double rateHz, qint64 baseUs, qint64 tUs, qint64 captureOffsetUs = 0)
{
    TimingSnapshot s;
    s.positionFrames = qint64(std::llround(rateHz * double(tUs) / 1e6));
    s.systemTimeUs = baseUs + tUs + captureOffsetUs;  // measured after capture
    s.captureOffsetUs = captureOffsetUs;
    s.valid = true;
    return s;
}
}  // namespace

class TstStreamclock : public QObject
{
    Q_OBJECT

private slots:
    void oneSnapshot_threeFragmentPositions_mapToDistinct20msSpacedTimes();
    void invalidSnapshot_isIgnored_clockStaysUnavailable();
    void fiftyPpmFastStream_tenMinutes_rateErrorMeasured_andMappingTracksDrift();
    void snapshotArrivingAfterItsData_stillMapsByPosition();
    void captureOffset_shiftsTimesEarlier_negativeMonitorOffsetShiftsLater();
    void roundTrip_positionAtOfTimeAt_isIdentity();
    void noTransportTerm_mappingDependsOnlyOnTimePositionAndCaptureOffset();
};

void TstStreamclock::oneSnapshot_threeFragmentPositions_mapToDistinct20msSpacedTimes()
{
    // One callback drained three 960-frame fragments and recorded one snapshot:
    // "the sample at position 2880 was captured at t = 5 000 000 µs".
    StreamClock clock;
    QVERIFY(!clock.isAvailable());
    clock.addSnapshot({5000000, 2880, 0, true});
    QVERIFY(clock.isAvailable());
    QCOMPARE(clock.snapshotCount(), 1);

    // Fragments at positions 0, 960, 1920 are 20 ms apart on the same anchor.
    QCOMPARE(clock.timeAtUs(2880), 5000000);
    QCOMPARE(clock.timeAtUs(1920), 5000000 - 20000);
    QCOMPARE(clock.timeAtUs(960), 5000000 - 40000);
    QCOMPARE(clock.timeAtUs(0), 5000000 - 60000);
}

void TstStreamclock::invalidSnapshot_isIgnored_clockStaysUnavailable()
{
    // A measurement with timestamp == 0 (or a corrupt write index) is pushed
    // with valid == false; the clock must not anchor on it.
    StreamClock clock;
    clock.addSnapshot({0, 4800, 0, false});
    QVERIFY(!clock.isAvailable());
    QCOMPARE(clock.snapshotCount(), 0);

    // ...and once available, a later invalid one must not disturb the anchor.
    clock.addSnapshot({1000000, 0, 0, true});
    clock.addSnapshot({0, 96000, 0, false});
    QCOMPARE(clock.snapshotCount(), 1);
    QCOMPARE(clock.timeAtUs(0), 1000000);
}

void TstStreamclock::fiftyPpmFastStream_tenMinutes_rateErrorMeasured_andMappingTracksDrift()
{
    // A stream producing 48000 * (1 + 50e-6) = 48002.4 frames per system second,
    // reported every 100 ms for 10 minutes (~ one snapshot per few callbacks).
    const double rateHz = 48000.0 * (1.0 + 50e-6);
    const qint64 base = 1'000'000'000;  // arbitrary CLOCK_MONOTONIC origin
    StreamClock clock;
    for (qint64 t = 0; t <= 30'000'000; t += 100'000)
        clock.addSnapshot(synth(rateHz, base, t));
    QCOMPARE(clock.rateError(), 0.0);   // span < 60 s: not yet reported
    for (qint64 t = 30'100'000; t <= 600'000'000; t += 100'000)
        clock.addSnapshot(synth(rateHz, base, t));

    QVERIFY(std::abs(clock.rateError() - 5e-5) < 1e-5);

    // After 10 min the stream is 600 s * 50 µs/s = 30 ms (1440 frames) ahead of
    // a frozen first anchor. The mapping must follow the latest anchor instead:
    // the sample at the true position for t = 600 s maps back to within a few
    // frames (EMA lag) of itself.
    const qint64 truePos = qint64(std::llround(rateHz * 600.0));
    const qint64 mappedPos = clock.positionAt(base + 600'000'000);
    QVERIFY2(std::abs(mappedPos - truePos) <= 5,
             qPrintable(QStringLiteral("mapped %1 vs true %2").arg(mappedPos).arg(truePos)));
    const qint64 mappedTime = clock.timeAtUs(truePos);
    QVERIFY2(std::abs(mappedTime - (base + 600'000'000)) <= 100,
             qPrintable(QStringLiteral("time %1 vs true %2").arg(mappedTime).arg(base + 600'000'000)));
}

void TstStreamclock::snapshotArrivingAfterItsData_stillMapsByPosition()
{
    // The consumer drains data at position 48000 before the snapshot describing
    // the stream around that position is pushed. Whenever the snapshot arrives,
    // the time of position 48000 is a function of position only: 1 s after the
    // sample at 0, regardless of the (much later) arrival moment.
    const qint64 base = 7'000'000;
    StreamClock clock;
    clock.addSnapshot(synth(48000.0, base, 0));
    // ... 48000 frames drained, nothing pushed to the clock, then a snapshot
    // taken at t = 1 s but delivered "now" (the clock has no notion of now).
    clock.addSnapshot(synth(48000.0, base, 1'000'000));
    QCOMPARE(clock.timeAtUs(48000), base + 1'000'000);
    QCOMPARE(clock.timeAtUs(24000), base + 500'000);
    QCOMPARE(clock.positionAt(base + 1'000'000), 48000);
}

void TstStreamclock::captureOffset_shiftsTimesEarlier_negativeMonitorOffsetShiftsLater()
{
    // Same measurement moment and position, but the server reports that the
    // sample at that position was captured 20 ms before the measurement.
    StreamClock plain, mic, monitor;
    plain.addSnapshot({10'000'000, 4800, 0, true});
    mic.addSnapshot({10'000'000, 4800, 20'000, true});
    QCOMPARE(mic.timeAtUs(4800), plain.timeAtUs(4800) - 20'000);
    QCOMPARE(mic.timeAtUs(0), 10'000'000 - 20'000 - 100'000);
    QCOMPARE(mic.positionAt(10'000'000 - 20'000), 4800);

    // A monitor whose sink latency exceeds the source latency reports a
    // negative offset: the sample was *heard* 5 ms after the measurement.
    monitor.addSnapshot({10'000'000, 4800, -5'000, true});
    QCOMPARE(monitor.timeAtUs(4800), 10'000'000 + 5'000);
    QCOMPARE(monitor.positionAt(10'000'000 + 5'000), 4800);
    QCOMPARE(monitor.timeAtUs(4800) - mic.timeAtUs(4800), 25'000);
}

void TstStreamclock::roundTrip_positionAtOfTimeAt_isIdentity()
{
    // 1e6 / 48000 is not an integer number of µs per frame (20.8333...), so
    // timeAtUs rounds; positionAt must round back to the exact frame for every
    // position, also after the anchor has been smoothed by several snapshots.
    StreamClock clock;
    clock.addSnapshot({123'456'789, 1000, 13'000, true});
    clock.addSnapshot({123'556'801, 5801, 13'000, true});  // 12 µs of jitter
    clock.addSnapshot({123'656'777, 10600, 12'900, true});
    for (qint64 p = -100; p < 200'000; p += 7)
        QCOMPARE(clock.positionAt(clock.timeAtUs(p)), p);
    for (qint64 p = 0; p < 48000; ++p)
        QCOMPARE(clock.positionAt(clock.timeAtUs(p)), p);
}

void TstStreamclock::noTransportTerm_mappingDependsOnlyOnTimePositionAndCaptureOffset()
{
    // Design statement: TimingSnapshot carries no transport (client<->server)
    // term and the clock adds none. Two snapshots that agree on systemTimeUs,
    // positionFrames and captureOffsetUs are therefore indistinguishable: a
    // measurement that differed only in transport delay is not representable
    // here and cannot shift the mapping. (Equal-field snapshots must therefore
    // produce identical mappings.)
    TimingSnapshot a;
    a.systemTimeUs = 42'000'000;
    a.positionFrames = 9'600;
    a.captureOffsetUs = 1'500;
    a.valid = true;
    TimingSnapshot b = a;   // "the same measurement, seen via a different transport"

    StreamClock ca, cb;
    ca.addSnapshot(a);
    cb.addSnapshot(b);
    for (qint64 p = 0; p < 100'000; p += 960)
        QCOMPARE(ca.timeAtUs(p), cb.timeAtUs(p));
    for (qint64 t = 41'000'000; t < 44'000'000; t += 12'345)
        QCOMPARE(ca.positionAt(t), cb.positionAt(t));

    // ...and each of the three fields does change the mapping:
    StreamClock cTime, cPos, cOff;
    TimingSnapshot d = a; d.systemTimeUs += 1'000;   cTime.addSnapshot(d);
    d = a; d.positionFrames += 48;                    cPos.addSnapshot(d);
    d = a; d.captureOffsetUs += 1'000;                cOff.addSnapshot(d);
    QCOMPARE(cTime.timeAtUs(0), ca.timeAtUs(0) + 1'000);
    QCOMPARE(cPos.timeAtUs(0), ca.timeAtUs(0) - 1'000);
    QCOMPARE(cOff.timeAtUs(0), ca.timeAtUs(0) - 1'000);
}

QTEST_GUILESS_MAIN(TstStreamclock)
#include "tst_streamclock.moc"
