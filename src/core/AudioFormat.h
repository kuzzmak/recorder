#pragma once

#include <cstddef>

/// The one PCM format used end to end: mono, 48 kHz, signed 16-bit, in
/// 960-frame (20 ms) chunks — the Opus frame size, so every chunk encodes
/// without buffering.
namespace AudioFormat {
constexpr int kRate = 48000;
constexpr int kChannels = 1;
constexpr int kFramesPerChunk = 960;
constexpr int kBytesPerFrame = 2 * kChannels;  // S16LE

constexpr std::size_t chunkBytes() { return std::size_t(kFramesPerChunk) * kBytesPerFrame; }
constexpr long long framesForMs(long long ms) { return ms * kRate / 1000; }
constexpr long long msForFrames(long long frames) { return frames * 1000 / kRate; }
}  // namespace AudioFormat
