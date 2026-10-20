#include "StreamAligner.h"

#include "AudioMixer.h"

#include <algorithm>
#include <cstring>
#include <deque>

namespace {
constexpr int kChunk = AudioFormat::kFramesPerChunk;

/// One capture stream as the aligner sees it: its queue, its clock and a
/// position-indexed buffer that is only ever consumed sequentially.
struct Stream {
    std::shared_ptr<SampleQueue> queue;
    StreamClock clock;
    std::deque<SampleQueue::Block> blocks;  ///< pending samples, ascending positions
    qint64 consumed = 0;                    ///< next position to consume
    qint64 frontSkip = 0;                   ///< frames of blocks.front() already consumed/dropped
    qint64 delivered = 0;                   ///< frames ever taken from the queue
    qint64 firstPos = 0;                    ///< position of the first sample ever delivered
    bool unplaced = false;                  ///< monitor absent at start: not yet on the timeline
    bool resyncPending = false;             ///< stalled: re-place by timestamp when data returns
    qint64 padRemaining = 0;                ///< silence still owed after an early resumption
    bool padding() const { return padRemaining >= kChunk; }

    TimingSnapshot lastSnapshot;            ///< last one handed to the clock

    /// Drain the queue into the buffer and refresh the clock.
    void pull()
    {
        if (!queue)
            return;
        const TimingSnapshot snap = queue->latestSnapshot();
        if (snap.valid
            && (snap.systemTimeUs != lastSnapshot.systemTimeUs
                || snap.positionFrames != lastSnapshot.positionFrames
                || snap.captureOffsetUs != lastSnapshot.captureOffsetUs)) {
            clock.addSnapshot(snap);
            lastSnapshot = snap;
        }
        for (auto &b : queue->take()) {
            if (b.samples.empty())
                continue;
            if (delivered == 0)
                firstPos = b.positionStart;
            delivered += qint64(b.samples.size());
            blocks.push_back(std::move(b));
        }
    }

    bool hasData() const { return !blocks.empty(); }

    /// Position of the first unconsumed sample in the front block.
    qint64 frontStart() const { return blocks.front().positionStart + frontSkip; }
    qint64 frontEnd() const
    {
        return blocks.front().positionStart + qint64(blocks.front().samples.size());
    }
    void popFront()
    {
        blocks.pop_front();
        frontSkip = 0;
    }

    /// Discard everything below `consumed`; returns the number of frames dropped.
    qint64 prune()
    {
        qint64 dropped = 0;
        while (!blocks.empty()) {
            if (frontEnd() <= consumed) {
                dropped += frontEnd() - frontStart();
                popFront();
            } else if (frontStart() < consumed) {
                const qint64 cut = consumed - frontStart();
                frontSkip += cut;
                dropped += cut;
                break;
            } else {
                break;
            }
        }
        return dropped;
    }

    /// Contiguous frames available starting exactly at `consumed`
    /// (prune() first: the buffer must not hold anything below it).
    qint64 available() const
    {
        qint64 n = 0;
        qint64 next = consumed;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const qint64 start = i == 0 ? frontStart() : blocks[i].positionStart;
            const qint64 end = blocks[i].positionStart + qint64(blocks[i].samples.size());
            if (start != next)
                break;
            n += end - start;
            next = end;
        }
        return n;
    }

    /// Advance `consumed` over `frames` buffered samples without emitting them.
    void skip(qint64 frames)
    {
        consumed += frames;
        prune();
    }

    /// Copy `frames` samples from `consumed` into dst and advance.
    void read(int16_t *dst, qint64 frames)
    {
        while (frames > 0) {
            const auto &b = blocks.front();
            const qint64 n = std::min(frames, qint64(b.samples.size()) - frontSkip);
            std::memcpy(dst, b.samples.data() + frontSkip, std::size_t(n) * sizeof(int16_t));
            dst += n;
            frames -= n;
            consumed += n;
            frontSkip += n;
            if (frontSkip == qint64(b.samples.size()))
                popFront();
        }
    }
};
}  // namespace

struct StreamAligner::Impl {
    Stream mic;
    Stream monitor;   // queue == nullptr when recording mic only
    AlignerConfig config;
    bool started = false;
    bool draining = false;
    bool timestampMode = false;   ///< both clocks available at start: resync + drift active
    bool driftCorrected = false;  ///< a drift correction already applied for the next chunk
    qint64 cursor = 0;         ///< output frames emitted
    qint64 micTrim = 0;        ///< mic position of output frame 0

    bool hasMonitor() const { return monitor.queue != nullptr; }

    bool tryStart(AlignerStats &stats)
    {
        if (mic.delivered < kChunk)
            return false;
        if (!hasMonitor()) {
            micTrim = mic.firstPos;
            stats.alignment = QStringLiteral("mic-only");
        } else if (monitor.delivered >= kChunk) {
            if (mic.clock.isAvailable() && monitor.clock.isAvailable()) {
                // Both first samples on the system clock; start at the later one.
                const qint64 t0 = std::max(mic.clock.timeAtUs(mic.firstPos),
                                           monitor.clock.timeAtUs(monitor.firstPos));
                micTrim = std::max(mic.firstPos, mic.clock.positionAt(t0));
                monitor.consumed = std::max(monitor.firstPos, monitor.clock.positionAt(t0));
                stats.alignment = QStringLiteral("timestamp");
                timestampMode = true;
            } else {
                // No usable clock: first arrival is frame 0 for both streams.
                micTrim = mic.firstPos;
                monitor.consumed = monitor.firstPos;
                stats.alignment = QStringLiteral("arrival-order");
            }
        } else if (draining || mic.delivered >= AudioFormat::framesForMs(config.startWaitMs)) {
            // The monitor did not show up: proceed mic-only and place the
            // monitor by timestamp when (if) it arrives.
            micTrim = mic.firstPos;
            monitor.unplaced = true;
            timestampMode = mic.clock.isAvailable();
            stats.alignment = timestampMode ? QStringLiteral("timestamp")
                                            : QStringLiteral("arrival-order");
        } else {
            return false;
        }
        mic.consumed = micTrim;
        stats.micTrim = micTrim;
        mic.prune();       // frames before T0 were never on the timeline:
        monitor.prune();   // skipped, not late
        started = true;
        return true;
    }

    /// First monitor data after a mic-only start: like a stall resumption
    /// from its first sample.
    void placeMonitor(AlignerStats &stats)
    {
        monitor.unplaced = false;
        monitor.consumed = monitor.firstPos;
        if (timestampMode && monitor.clock.isAvailable())
            resync(monitor, desiredMon(), stats.monitorLateDroppedFrames);
    }

    qint64 stallFrames() const { return AudioFormat::framesForMs(config.stallMs); }

    qint64 desiredMic() const { return micTrim + cursor; }
    qint64 desiredMon() const
    {
        return monitor.clock.positionAt(mic.clock.timeAtUs(micTrim + cursor));
    }

    /// A stalled stream has data again: re-place it on the timeline. Its
    /// consumed position is never rewound.
    void resync(Stream &s, qint64 desired, qint64 &lateDropped)
    {
        s.resyncPending = false;
        if (!timestampMode)
            return;   // arrival order: resumed samples just follow the silence
        const qint64 delta = desired - s.consumed;
        if (delta > 0) {
            s.consumed += delta;   // watermark: frames below it are late
            lateDropped += s.prune();
        } else if (delta < 0) {
            s.padRemaining = -delta;   // ahead of target: hold it back with silence
        }
    }

    /// One chunk of resync padding for `s` (not advancing it) if it owes any.
    static bool takePad(Stream &s, qint64 &padFrames)
    {
        if (!s.padding())
            return false;
        s.padRemaining -= kChunk;
        padFrames += kChunk;
        return true;
    }

    /// Take one chunk from each stream that is "live" (silence for the other),
    /// mix and advance the output cursor.
    void emitChunk(int16_t *out, bool micLive, bool monLive, AlignerStats &stats)
    {
        int16_t micPcm[kChunk], monPcm[kChunk];
        if (micLive)
            mic.read(micPcm, kChunk);
        else
            std::memset(micPcm, 0, sizeof micPcm);
        if (monLive)
            monitor.read(monPcm, kChunk);
        else
            std::memset(monPcm, 0, sizeof monPcm);
        AudioMixer::mix(out, micPcm, monPcm, kChunk, config.micGain, config.systemGain);
        cursor += kChunk;
        ++stats.chunksEmitted;
        driftCorrected = false;
    }

    /// Drift accumulator (monitor only): one correction per chunk, and only
    /// while no resync watermark / padding is pending. Returns true when this
    /// chunk gets monitor silence instead (negative drift).
    bool applyDrift(AlignerStats &stats)
    {
        if (!timestampMode || driftCorrected || monitor.resyncPending || monitor.padding())
            return false;
        const qint64 err = desiredMon() - monitor.consumed;
        if (err >= config.driftThresholdFrames) {
            monitor.skip(kChunk);
            stats.driftDropFrames += kChunk;
            driftCorrected = true;
        } else if (err <= -config.driftThresholdFrames) {
            stats.driftPadFrames += kChunk;
            driftCorrected = true;
            return true;
        }
        return false;
    }

    bool emitBoth(int16_t *out, AlignerStats &stats)
    {
        if (monitor.unplaced) {
            if (monitor.hasData()) {
                placeMonitor(stats);
            } else {
                if (mic.available() < kChunk)
                    return false;
                emitChunk(out, true, false, stats);
                ++stats.monitorStalls;
                return true;
            }
        }
        if (mic.resyncPending && mic.hasData())
            resync(mic, desiredMic(), stats.micLateDroppedFrames);
        if (monitor.resyncPending && monitor.hasData())
            resync(monitor, desiredMon(), stats.monitorLateDroppedFrames);
        const qint64 micAvail = mic.available();
        const qint64 monAvail = monitor.available();
        const bool micReady = micAvail >= kChunk || mic.padding();
        const bool monReady = monAvail >= kChunk || monitor.padding();
        if (micReady && monReady) {
            const bool driftPad = applyDrift(stats);
            if (!driftPad && !monitor.padding() && monitor.available() < kChunk)
                return false;   // the skip consumed the only buffered monitor chunk
            const bool micPad = takePad(mic, stats.micResyncPadFrames);
            const bool monPad = !driftPad && takePad(monitor, stats.monitorResyncPadFrames);
            emitChunk(out, !micPad, !(monPad || driftPad), stats);
            return true;
        }
        // Stall: one stream has nothing to emit while the other has accumulated
        // stallMs of audio beyond its own consumed position.
        if (monAvail < kChunk && micAvail >= stallFrames()) {
            emitChunk(out, true, false, stats);
            ++stats.monitorStalls;
            monitor.resyncPending = true;
            return true;
        }
        if (micAvail < kChunk && monAvail >= stallFrames()) {
            emitChunk(out, false, true, stats);
            ++stats.micStalls;
            mic.resyncPending = true;
            return true;
        }
        return false;
    }

    /// Queues are closed: every chunk the mic can still fill comes out, with
    /// the monitor sequentially as far as it goes and silence after.
    bool emitDraining(int16_t *out, AlignerStats &stats)
    {
        if (mic.available() < kChunk)
            return false;   // the sub-chunk tail is discarded
        if (!hasMonitor()) {
            emitChunk(out, true, false, stats);
            return true;
        }
        if (monitor.unplaced && monitor.hasData())
            placeMonitor(stats);
        // The two streams are stopped at slightly different moments, so the
        // monitor usually ends a chunk or two short of the mic: substituting
        // silence for that tail is normal and is not reported as a gap.
        const bool monLive = !monitor.unplaced && monitor.available() >= kChunk;
        emitChunk(out, true, monLive, stats);
        return true;
    }

    bool emitMicOnly(int16_t *out, AlignerStats &stats)
    {
        if (mic.available() < kChunk)
            return false;
        emitChunk(out, true, false, stats);
        return true;
    }
};

StreamAligner::StreamAligner(std::shared_ptr<SampleQueue> mic,
                             std::shared_ptr<SampleQueue> monitor,
                             AlignerConfig config)
    : d(new Impl)
{
    d->mic.queue = std::move(mic);
    d->monitor.queue = std::move(monitor);
    d->config = config;
}

StreamAligner::~StreamAligner() = default;

bool StreamAligner::nextChunk(int16_t *out)
{
    d->mic.pull();
    d->monitor.pull();
    if (!d->started && !d->tryStart(m_stats))
        return false;
    // Anything that arrives below a consumed position is for an interval that
    // has already been emitted (a persisting resync watermark included).
    m_stats.micLateDroppedFrames += d->mic.prune();
    m_stats.monitorLateDroppedFrames += d->monitor.prune();
    if (d->draining)
        return d->emitDraining(out, m_stats);
    if (!d->hasMonitor())
        return d->emitMicOnly(out, m_stats);
    return d->emitBoth(out, m_stats);
}

void StreamAligner::beginDrain()
{
    d->draining = true;
}
