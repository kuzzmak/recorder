#pragma once

#include "AudioFormat.h"
#include "SampleQueue.h"
#include "StreamClock.h"

#include <QString>
#include <cstdint>
#include <memory>

/// Alignment knobs (defaults are the documented policy).
struct AlignerConfig {
    int startWaitMs = 1000;           ///< mic audio to buffer before giving up on the monitor's first chunk
    int stallMs = 500;                ///< the other stream's progress that declares a stream stalled
    int driftThresholdFrames = 4800;  ///< |err| that triggers one 960-frame drift correction (100 ms)
    double micGain = 1.0;
    double systemGain = 1.0;
};

/// Counters reported with the finished recording.
struct AlignerStats {
    qint64 micTrim = 0;               ///< mic frames discarded before output frame 0
    int monitorStalls = 0;            ///< chunks where the monitor was substituted with silence
    int micStalls = 0;                ///< chunks where the mic was substituted with silence
    qint64 micLateDroppedFrames = 0;  ///< frames that arrived for an already-emitted interval
    qint64 monitorLateDroppedFrames = 0;
    qint64 micResyncPadFrames = 0;    ///< silence inserted to let an early-resuming stream catch up
    qint64 monitorResyncPadFrames = 0;
    qint64 driftPadFrames = 0;        ///< monitor silence inserted by the drift accumulator
    qint64 driftDropFrames = 0;       ///< monitor frames skipped by the drift accumulator
    qint64 chunksEmitted = 0;
    QString alignment;                ///< "timestamp" | "arrival-order" | "mic-only"

    qint64 lateDroppedFrames() const { return micLateDroppedFrames + monitorLateDroppedFrames; }
    int gapsPatched() const { return monitorStalls + micStalls; }
};

/// Pulls samples from the mic queue (and the optional monitor queue), places
/// both on the microphone's timeline by stream position, and emits mixed
/// 960-frame chunks. Pure: no threads, no I/O, deterministic for a given
/// sequence of pushes. See the README ("Alignment policy") for the rules:
/// one sequential consumption rule per stream, stall + resync with a discard
/// watermark / padding budget, and a monitor-only drift accumulator.
class StreamAligner
{
public:
    StreamAligner(std::shared_ptr<SampleQueue> mic,
                  std::shared_ptr<SampleQueue> monitor,  ///< nullptr => mic only
                  AlignerConfig config = {});
    ~StreamAligner();

    /// Produce the next chunk (AudioFormat::kFramesPerChunk frames) into `out`.
    /// Returns false when nothing can be emitted yet (or ever, after drain).
    bool nextChunk(int16_t *out);
    /// Queues are closed: from now on emit what is left (monitor sequentially
    /// as available, else silence) and discard the sub-chunk tail.
    void beginDrain();

    const AlignerStats &stats() const { return m_stats; }
    qint64 outputFrames() const { return m_stats.chunksEmitted * AudioFormat::kFramesPerChunk; }

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    AlignerStats m_stats;
};
