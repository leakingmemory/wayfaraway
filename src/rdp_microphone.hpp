#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

struct wl_event_loop;
struct wl_event_source;
typedef struct s_audin_server_context audin_server_context;
typedef struct rdp_context rdpContext;

namespace wfa {

// The audin (audio input) dynamic channel of one RDP peer: the client's
// microphone. The channel is only open while set_open(true), which the owner
// ties to "unlocked and an application is recording".
//
// FreeRDP runs audin on its own thread; samples are handed back to the
// Wayland event loop through an eventfd and delivered in audio:: format.
class RdpMicrophone {
public:
    using Callback = std::function<void(std::span<const std::byte> samples)>;

    static std::unique_ptr<RdpMicrophone> create(wl_event_loop* loop, void* vcm, rdpContext* context, Callback callback);
    ~RdpMicrophone();

    RdpMicrophone(const RdpMicrophone&) = delete;
    RdpMicrophone& operator=(const RdpMicrophone&) = delete;

    void set_open(bool open);

    explicit RdpMicrophone(Callback callback);

private:
    struct Callbacks;

    bool init(wl_event_loop* loop, void* vcm, rdpContext* context);
    static int on_eventfd(int fd, uint32_t mask, void* data);

    Callback m_callback;
    audin_server_context* m_context = nullptr;
    bool m_open = false;

    int m_eventfd = -1;
    wl_event_source* m_source = nullptr;

    // Shared with the audin thread.
    std::mutex m_mutex;
    std::vector<std::byte> m_pending;
};

} // namespace wfa
