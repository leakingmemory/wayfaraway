#pragma once

#include <cstdint>

namespace wfa::audio {

// The one PCM format used between PipeWire and RDP, in both directions:
// interleaved signed 16-bit little-endian stereo at 44.1 kHz.
inline constexpr std::uint32_t rate = 44100;
inline constexpr std::uint32_t channels = 2;
inline constexpr std::uint32_t bytes_per_sample = 2;
inline constexpr std::uint32_t bytes_per_frame = channels * bytes_per_sample;

} // namespace wfa::audio
