#include "audio_sink.hpp"

#include <algorithm>
#include <cstdint>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "wlr.hpp"

namespace wfa {

struct AudioSink::Impl {
    Callback callback;

    pw_loop* loop = nullptr;
    bool entered = false;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook core_listener{};
    spa_hook stream_listener{};
    wl_event_source* source = nullptr;

    ~Impl()
    {
        if (stream)
            pw_stream_destroy(stream);
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

    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* error)
    {
        if (state == PW_STREAM_STATE_ERROR)
            wlr_log(WLR_ERROR, "Audio: sink stream failed: %s", error ? error : "unknown error");
        else
            wlr_log(WLR_DEBUG, "Audio: sink stream %s", pw_stream_state_as_string(state));
    }

    // Without PW_STREAM_FLAG_RT_PROCESS this runs on the main (Wayland) loop.
    static void on_process(void* data)
    {
        auto* self = static_cast<Impl*>(data);
        pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
        if (!buffer)
            return;

        const spa_data& d = buffer->buffer->datas[0];
        if (d.data && d.chunk) {
            const std::uint32_t offset = std::min(d.chunk->offset, d.maxsize);
            const std::uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
            std::span samples(static_cast<const std::byte*>(d.data) + offset, size - size % bytes_per_frame);

            // Pure silence is not worth sending over the network.
            const bool silent = std::ranges::all_of(samples, [](std::byte b) { return b == std::byte{ 0 }; });
            if (!samples.empty() && !silent && !(d.chunk->flags & SPA_CHUNK_FLAG_EMPTY))
                self->callback(samples);
        }
        pw_stream_queue_buffer(self->stream, buffer);
    }

    static constexpr pw_core_events core_events = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.error = &Impl::on_core_error;
        return events;
    }();

    static constexpr pw_stream_events stream_events = [] {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &Impl::on_state_changed;
        events.process = &Impl::on_process;
        return events;
    }();
};

std::unique_ptr<AudioSink> AudioSink::create(wl_event_loop* loop, Callback callback)
{
    static const bool initialised = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)initialised;

    auto impl = std::make_unique<Impl>();
    impl->callback = std::move(callback);

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

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_NAME, "wayfaraway-rdp",
        PW_KEY_NODE_DESCRIPTION, "Remote desktop (RDP)",
        PW_KEY_NODE_WANT_DRIVER, "true",
        // Above typical hardware sinks, so the session manager makes this the
        // default output; on a headless machine it is usually the only one.
        PW_KEY_PRIORITY_SESSION, "2000",
        "audio.position", "FL,FR",
        nullptr);
    impl->stream = pw_stream_new(impl->core, "wayfaraway RDP audio", props);
    if (!impl->stream)
        return nullptr;
    pw_stream_add_listener(impl->stream, &impl->stream_listener, &Impl::stream_events, impl.get());

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16_LE;
    info.rate = rate;
    info.channels = channels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;

    std::uint8_t pod_buffer[1024];
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_buffer, sizeof(pod_buffer));
    const spa_pod* params[] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };

    // A sink is linked to by the session manager, so no AUTOCONNECT.
    if (pw_stream_connect(impl->stream, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
        wlr_log(WLR_ERROR, "Audio: cannot create the PipeWire sink");
        return nullptr;
    }

    wlr_log(WLR_INFO, "Audio: PipeWire sink 'wayfaraway-rdp' ready");
    return std::make_unique<AudioSink>(std::move(impl));
}

AudioSink::AudioSink(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

AudioSink::~AudioSink() = default;

} // namespace wfa
