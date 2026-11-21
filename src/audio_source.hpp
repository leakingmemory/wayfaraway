#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>

namespace wfa {

class PipeWire;

// A PipeWire source node ("Remote desktop microphone") that applications
// can record from. Like AudioSink it outlives RDP connections. Samples
// pushed in audio:: format are played out to recorders; underruns are
// filled with silence.
//
// It reports whether anything is recording, so the client's microphone is
// only opened while it is actually needed.
class AudioSource {
public:
    using DemandCallback = std::function<void(bool recording)>;

    static std::unique_ptr<AudioSource> create(PipeWire& pipewire, DemandCallback on_demand);
    ~AudioSource();

    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;

    bool recording() const;
    void push(std::span<const std::byte> samples);
    // Drops buffered audio, e.g. when the microphone closes.
    void flush();

    struct Impl;
    explicit AudioSource(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace wfa
