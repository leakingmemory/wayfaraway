#include "rdp_display.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <freerdp/server/disp.h>
#include <winpr/wtsapi.h>

#include "wlr.hpp"

namespace wfa {

namespace {

// What we tell the client it may ask for; set_monitor_layout clamps anyway.
constexpr UINT32 max_monitors = 16;
constexpr UINT32 max_monitor_size = 8192;

} // namespace

// Runs on FreeRDP's disp thread.
struct RdpDisplayControl::Callbacks {
    static UINT monitor_layout(DispServerContext* context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU* pdu)
    {
        auto* self = static_cast<RdpDisplayControl*>(context->custom);

        std::vector<Monitor> monitors;
        for (UINT32 i = 0; i < pdu->NumMonitors && i < max_monitors; ++i) {
            const DISPLAY_CONTROL_MONITOR_LAYOUT& m = pdu->Monitors[i];
            monitors.push_back({
                .x = m.Left,
                .y = m.Top,
                .width = static_cast<int>(m.Width),
                .height = static_cast<int>(m.Height),
                .primary = (m.Flags & DISPLAY_CONTROL_MONITOR_PRIMARY) != 0,
            });
        }
        if (monitors.empty())
            return CHANNEL_RC_OK;

        {
            std::scoped_lock lock(self->m_mutex);
            self->m_pending = std::move(monitors);
        }
        std::uint64_t one = 1;
        [[maybe_unused]] auto n = write(self->m_eventfd, &one, sizeof(one));
        return CHANNEL_RC_OK;
    }
};

std::unique_ptr<RdpDisplayControl> RdpDisplayControl::create(wl_event_loop* loop, void* vcm, rdpContext* context,
    Callback callback)
{
    auto display = std::make_unique<RdpDisplayControl>(std::move(callback));
    if (!display->init(loop, vcm, context))
        return nullptr;
    return display;
}

RdpDisplayControl::RdpDisplayControl(Callback callback)
    : m_callback(std::move(callback))
{
}

bool RdpDisplayControl::init(wl_event_loop* loop, void* vcm, rdpContext* context)
{
    m_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_eventfd < 0)
        return false;
    m_source = wl_event_loop_add_fd(loop, m_eventfd, WL_EVENT_READABLE, &RdpDisplayControl::on_eventfd, this);
    if (!m_source)
        return false;

    m_context = disp_server_context_new(vcm);
    if (!m_context)
        return false;
    m_context->custom = this;
    m_context->rdpcontext = context;
    m_context->MaxNumMonitors = max_monitors;
    m_context->MaxMonitorAreaFactorA = max_monitor_size;
    m_context->MaxMonitorAreaFactorB = max_monitor_size;
    m_context->DispMonitorLayout = &Callbacks::monitor_layout;
    return true;
}

RdpDisplayControl::~RdpDisplayControl()
{
    // Must not hold m_mutex here: Close waits for the disp thread.
    if (m_open)
        (void)m_context->Close(m_context);
    if (m_context)
        disp_server_context_free(m_context);
    if (m_source)
        wl_event_source_remove(m_source);
    if (m_eventfd >= 0)
        close(m_eventfd);
}

void RdpDisplayControl::open()
{
    if (m_open || !m_context)
        return;
    if (m_context->Open(m_context) != CHANNEL_RC_OK) {
        wlr_log(WLR_ERROR, "Display: cannot open the display control channel");
        return;
    }
    m_open = true;
    // Advertising our limits is what lets the client start sending layouts.
    if (m_context->DisplayControlCaps(m_context) != CHANNEL_RC_OK)
        wlr_log(WLR_ERROR, "Display: cannot send display control capabilities");
    else
        wlr_log(WLR_INFO, "Display: dynamic monitor layout enabled");
}

int RdpDisplayControl::on_eventfd(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<RdpDisplayControl*>(data);
    std::uint64_t value = 0;
    [[maybe_unused]] auto n = read(fd, &value, sizeof(value));

    std::optional<std::vector<Monitor>> monitors;
    {
        std::scoped_lock lock(self->m_mutex);
        monitors.swap(self->m_pending);
    }
    if (monitors && self->m_callback)
        self->m_callback(std::move(*monitors));
    return 0;
}

} // namespace wfa
