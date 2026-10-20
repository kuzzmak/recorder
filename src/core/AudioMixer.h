#pragma once

#include <cstddef>
#include <cstdint>

/// Pure sample arithmetic shared by the aligner and the level meter.
class AudioMixer
{
public:
    /// dst[i] = clip(a[i] * gainA + b[i] * gainB). `b` may be null (pass-through
    /// of `a` with gainA). dst may alias a.
    static void mix(int16_t *dst, const int16_t *a, const int16_t *b, std::size_t frames,
                    double gainA = 1.0, double gainB = 1.0);
    /// Largest absolute sample value, 0..32768.
    static int peak(const int16_t *pcm, std::size_t frames);
};
