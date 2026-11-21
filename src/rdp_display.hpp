#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "server.hpp"

struct wl_event_loop;
struct wl_event_source;
typedef struct s_disp_server_context DispServerContext;
typedef struct rdp_context rdpContext;

namespace wfa {

// The Display Control dynamic channel of one RDP peer: the client asks for a
// new monitor layout when its window is resized or monitors are added or
// removed. FreeRDP runs the channel on its own thread; layouts are handed to
// the Wayland event loop through an eventfd, newest wins.
class RdpDisplayControl {
public:
    using Callback = std::function<void(std::vector<Monitor> monitors)>;

    static std::unique_ptr<RdpDisplayControl> create(wl_event_loop* loop, void* vcm, rdpContext* context,
        Callback callback);
    ~RdpDisplayControl();

    RdpDisplayControl(const RdpDisplayControl&) = delete;
    RdpDisplayControl& operator=(const RdpDisplayControl&) = delete;

    // Opens the channel; only valid once drdynvc is ready.
    void open();

    explicit RdpDisplayControl(Callback callback);

private:
    struct Callbacks;

    bool init(wl_event_loop* loop, void* vcm, rdpContext* context);
    static int on_eventfd(int fd, uint32_t mask, void* data);

    Callback m_callback;
    DispServerContext* m_context = nullptr;
    bool m_open = false;

    int m_eventfd = -1;
    wl_event_source* m_source = nullptr;

    // Shared with FreeRDP's disp thread.
    std::mutex m_mutex;
    std::optional<std::vector<Monitor>> m_pending;
};

} // namespace wfa
