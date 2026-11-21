#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

struct wl_event_loop;
struct wl_event_source;
typedef struct s_rdpsnd_server_context RdpsndServerContext;

namespace wfa {

// The rdpsnd (audio output) channel of one RDP peer. Samples are in
// AudioSink's format; FreeRDP converts them to the client's chosen format.
// Nothing is played while the session is locked.
class RdpAudio {
public:
    // `vcm` is the peer's WTS virtual channel manager. Returns nullptr if the
    // channel cannot be started.
    static std::unique_ptr<RdpAudio> create(wl_event_loop* loop, void* vcm, bool locked);
    ~RdpAudio();

    RdpAudio(const RdpAudio&) = delete;
    RdpAudio& operator=(const RdpAudio&) = delete;

    void set_locked(bool locked) { m_locked = locked; }
    void push(std::span<const std::byte> samples);

    explicit RdpAudio(bool locked);

private:
    bool start(wl_event_loop* loop, void* vcm);
    static int on_channel_event(int fd, uint32_t mask, void* data);
    static void on_activated(RdpsndServerContext* context);

    RdpsndServerContext* m_context = nullptr;
    wl_event_source* m_source = nullptr;
    bool m_format_selected = false;
    bool m_locked;
};

} // namespace wfa
