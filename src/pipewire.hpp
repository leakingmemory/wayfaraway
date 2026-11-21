#pragma once

#include <memory>

struct wl_event_loop;
struct pw_core;

namespace wfa {

// One connection to the PipeWire daemon, driven from the Wayland event loop
// through the pw_loop fd (no extra threads). Shared by the audio sink and
// source; they must be destroyed before it.
class PipeWire {
public:
    // Returns nullptr if PipeWire is not running.
    static std::unique_ptr<PipeWire> connect(wl_event_loop* loop);
    ~PipeWire();

    PipeWire(const PipeWire&) = delete;
    PipeWire& operator=(const PipeWire&) = delete;

    pw_core* core() const;

    struct Impl;
    explicit PipeWire(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace wfa
