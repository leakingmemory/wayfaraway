#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

struct wl_event_loop;

namespace wfa {

// A PipeWire sink node ("Remote desktop (RDP)") that applications can play
// to. It lives as long as the compositor, so audio routing survives RDP
// reconnects; whatever is played is handed to a callback, which forwards it
// to the active peer or drops it.
//
// PipeWire is driven from the Wayland event loop; no extra threads.
class AudioSink {
public:
    // Fixed capture format: interleaved signed 16-bit little-endian stereo.
    static constexpr std::uint32_t rate = 44100;
    static constexpr std::uint32_t channels = 2;
    static constexpr std::uint32_t bytes_per_frame = channels * 2;

    using Callback = std::function<void(std::span<const std::byte> samples)>;

    // Returns nullptr if PipeWire is not reachable.
    static std::unique_ptr<AudioSink> create(wl_event_loop* loop, Callback callback);
    ~AudioSink();

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    struct Impl;
    explicit AudioSink(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace wfa
