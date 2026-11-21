#include "pipewire.hpp"

#include <pipewire/pipewire.h>

#include "wlr.hpp"

namespace wfa {

struct PipeWire::Impl {
    pw_loop* loop = nullptr;
    bool entered = false;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    spa_hook core_listener{};
    wl_event_source* source = nullptr;

    ~Impl()
    {
        if (core)
            pw_core_disconnect(core);
        if (context)
            pw_context_destroy(context);
        if (source)
            wl_event_source_remove(source);
        if (entered)
            pw_loop_leave(loop);
        if (loop)
            pw_loop_destroy(loop);
    }

    static int on_loop_fd(int fd, uint32_t mask, void* data)
    {
        pw_loop_iterate(static_cast<Impl*>(data)->loop, 0);
        return 0;
    }

    static void on_core_error(void* data, uint32_t id, int seq, int res, const char* message)
    {
        wlr_log(WLR_ERROR, "Audio: PipeWire error on object %u: %s", id, message);
    }

    static constexpr pw_core_events core_events = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.error = &Impl::on_core_error;
        return events;
    }();
};

std::unique_ptr<PipeWire> PipeWire::connect(wl_event_loop* loop)
{
    static const bool initialised = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)initialised;

    auto impl = std::make_unique<Impl>();
    impl->loop = pw_loop_new(nullptr);
    if (!impl->loop)
        return nullptr;
    pw_loop_enter(impl->loop);
    impl->entered = true;
    impl->source = wl_event_loop_add_fd(loop, pw_loop_get_fd(impl->loop), WL_EVENT_READABLE, &Impl::on_loop_fd, impl.get());

    impl->context = pw_context_new(impl->loop, nullptr, 0);
    if (!impl->context)
        return nullptr;
    impl->core = pw_context_connect(impl->context, nullptr, 0);
    if (!impl->core) {
        wlr_log(WLR_INFO, "Audio: PipeWire is not running; audio disabled");
        return nullptr;
    }
    pw_core_add_listener(impl->core, &impl->core_listener, &Impl::core_events, impl.get());
    return std::make_unique<PipeWire>(std::move(impl));
}

PipeWire::PipeWire(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

PipeWire::~PipeWire() = default;

pw_core* PipeWire::core() const
{
    return m_impl->core;
}

} // namespace wfa
