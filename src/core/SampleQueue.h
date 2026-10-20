#pragma once

#include "AudioFormat.h"
#include "TimingSnapshot.h"

#include <QString>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

/// Thread-safe FIFO carrying PCM16 mono blocks from one capture stream
/// (producer: the pulse mainloop thread) to the encoder (single consumer).
///
/// Each block remembers the stream position (frames since stream start,
/// holes included) of its first sample, so the consumer can place it on a
/// timeline independently of when it arrived. Holes are pushed as silence.
///
/// Capacity is bounded: a push that would exceed `maxFrames` is refused
/// (returns false) and closes the queue with an overflow error — an explicit
/// termination rather than silent data loss, because 30 s of backlog means
/// the consumer is stuck. After `close()` every push is ignored.
///
/// Lock discipline: the mutex is held only for the push / take itself, never
/// across I/O; nothing holding it ever calls into libpulse.
class SampleQueue
{
public:
    struct Block {
        qint64 positionStart = 0;       ///< stream position of samples[0]
        std::vector<int16_t> samples;   ///< mono S16, one entry per frame
    };

    static constexpr qint64 kDefaultMaxFrames = qint64(AudioFormat::kRate) * 30;  // 30 s
    static constexpr const char *kOverflowError = "Audio buffer overflow: encoder stalled";

    explicit SampleQueue(qint64 maxFrames = kDefaultMaxFrames);

    // --- producer side -----------------------------------------------------
    /// Append `frames` samples starting at stream position `positionStart`.
    /// Returns false (and closes with kOverflowError) when the queue is full,
    /// or false without side effects when already closed.
    bool push(const int16_t *samples, std::size_t frames, qint64 positionStart);
    /// Same as push() with zeros (a server-reported hole).
    bool pushSilence(std::size_t frames, qint64 positionStart);
    /// Record the newest timing measurement (invalid snapshots are kept too;
    /// the clock decides what to do with them).
    void pushSnapshot(const TimingSnapshot &snapshot);
    /// Mark end of stream. Empty `error` is a clean stop; a non-empty one is a
    /// fatal reason the consumer reports. Idempotent; the first reason wins.
    void close(const QString &error = QString());

    // --- consumer side -----------------------------------------------------
    /// Move every pending block out (oldest first); the queue is empty after.
    std::vector<Block> take();
    /// Block until data is pending or the queue is closed, at most timeoutMs.
    /// Returns true if either condition holds, false on timeout.
    bool waitForChange(int timeoutMs);
    TimingSnapshot latestSnapshot() const;

    // --- inspection (any thread) -------------------------------------------
    bool isClosed() const;
    QString error() const;          ///< non-empty only after an error close
    qint64 pendingFrames() const;
    qint64 maxFrames() const { return m_maxFrames; }

private:
    const qint64 m_maxFrames;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Block> m_blocks;
    qint64 m_pendingFrames = 0;
    TimingSnapshot m_snapshot;
    bool m_closed = false;
    QString m_error;
};
