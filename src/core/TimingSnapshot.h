#pragma once

#include <QtGlobal>

/// libpulse-free view of one `pa_timing_info` measurement from a capture
/// stream: "the sample at `positionFrames` was captured (for a monitor:
/// heard) at `systemTimeUs - captureOffsetUs`".
///
/// - systemTimeUs: `ti->timestamp`, the client-clock estimate of the moment
///   the server took the measurement (CLOCK_MONOTONIC-ish µs; only differences
///   matter, no transport term is included — see the README's alignment notes).
/// - positionFrames: `ti->write_index / bytesPerFrame`, i.e. the newest sample
///   in the server's record buffer at that moment, in the same index space as
///   SampleQueue positions (holes advance it too).
/// - captureOffsetUs: `source_usec` (minus `sink_usec` for a monitor stream);
///   signed, may be negative for a monitor whose sink latency exceeds the
///   source latency.
/// - valid: false when the measurement must be ignored (corrupt write index
///   or `timestamp == 0`).
struct TimingSnapshot {
    qint64 systemTimeUs = 0;
    qint64 positionFrames = 0;
    qint64 captureOffsetUs = 0;
    bool valid = false;
};
