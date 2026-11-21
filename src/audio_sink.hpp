#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>

namespace wfa {

class PipeWire;

// A PipeWire sink node ("Remote desktop (RDP)") that applications can play
// to. It lives as long as the compositor, so audio routing survives RDP
// reconnects; whatever is played (in audio:: format) is handed to a
// callback, which forwards it to the active peer or drops it.
class AudioSink {
public:
    using Callback = std::function<void(std::span<const std::byte> samples)>;

    static std::unique_ptr<AudioSink> create(PipeWire& pipewire, Callback callback);
    ~AudioSink();

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    struct Impl;
    explicit AudioSink(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace wfa
