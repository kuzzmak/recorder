#pragma once

#include "AudioFormat.h"
#include "TimingSnapshot.h"

/// Maps one stream's sample positions onto system time and back.
///
/// A snapshot says "the sample at `positionFrames` was captured (heard, for a
/// monitor) at `systemTimeUs - captureOffsetUs`". From that anchor:
///   timeAtUs(p)   = anchorTimeUs + (p - anchorPos) * 1e6 / rate
///   positionAt(t) = anchorPos + (t - anchorTimeUs) * rate / 1e6
/// New snapshots refine the anchor with exponential smoothing of the implied
/// offset (per-snapshot jitter is a few ms; every consumer threshold is
/// >= 100 ms). Until the first valid snapshot the clock is Unavailable and
/// the mappings are undefined (callers must check isAvailable()).
///
/// rateError() = measured frames per system second / rate - 1, reported once
/// snapshots span >= 60 s (0.0 before). Informational: the mapping itself
/// tracks drift through the moving anchor.
class StreamClock
{
public:
    explicit StreamClock(int rate = AudioFormat::kRate);

    void addSnapshot(const TimingSnapshot &snapshot);  ///< invalid ones are ignored
    bool isAvailable() const;
    qint64 timeAtUs(qint64 positionFrames) const;
    qint64 positionAt(qint64 systemTimeUs) const;
    double rateError() const;
    int snapshotCount() const { return m_count; }

private:
    static constexpr double kSmoothing = 0.1;  ///< EMA weight of each new snapshot after the first

    int m_rate;
    int m_count = 0;
    qint64 m_anchorPos = 0;
    qint64 m_anchorTimeUs = 0;
    double m_offsetUs = 0.0;      ///< smoothed (captureTime - pos * 1e6 / rate)
    qint64 m_firstPos = 0;        ///< first valid snapshot, for rateError()
    qint64 m_firstTimeUs = 0;
    qint64 m_latestPos = 0;
    qint64 m_latestTimeUs = 0;
};
