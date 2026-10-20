#include "AudioMixer.h"

#include <cmath>

namespace {
int16_t clip(double v)
{
    if (v > 32767.0)
        return 32767;
    if (v < -32768.0)
        return -32768;
    return int16_t(std::lrint(v));
}
}  // namespace

void AudioMixer::mix(int16_t *dst, const int16_t *a, const int16_t *b, std::size_t frames,
                     double gainA, double gainB)
{
    if (!b) {
        for (std::size_t i = 0; i < frames; ++i)
            dst[i] = clip(a[i] * gainA);
        return;
    }
    for (std::size_t i = 0; i < frames; ++i)
        dst[i] = clip(a[i] * gainA + b[i] * gainB);
}

int AudioMixer::peak(const int16_t *pcm, std::size_t frames)
{
    int peak = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        const int v = pcm[i] < 0 ? -int(pcm[i]) : int(pcm[i]);  // int so -32768 => 32768
        if (v > peak)
            peak = v;
    }
    return peak;
}
