#include "audio_sink.hpp"

#include <algorithm>
#include <cstdint>

#include <pipewire/pipewire.h>

#include "audio_format.hpp"
#include "pipewire.hpp"
#include "pipewire_format.hpp"
#include "wlr.hpp"

namespace wfa {

struct AudioSink::Impl {
    Callback callback;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};

    ~Impl()
    {
        if (stream)
            pw_stream_destroy(stream);
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
            std::span samples(static_cast<const std::byte*>(d.data) + offset, size - size % audio::bytes_per_frame);

            // Pure silence is not worth sending over the network.
            const bool silent = std::ranges::all_of(samples, [](std::byte b) { return b == std::byte{ 0 }; });
            if (!samples.empty() && !silent && !(d.chunk->flags & SPA_CHUNK_FLAG_EMPTY))
                self->callback(samples);
        }
        pw_stream_queue_buffer(self->stream, buffer);
    }

    static constexpr pw_stream_events stream_events = [] {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &Impl::on_state_changed;
        events.process = &Impl::on_process;
        return events;
    }();
};

std::unique_ptr<AudioSink> AudioSink::create(PipeWire& pipewire, Callback callback)
{
    auto impl = std::make_unique<Impl>();
    impl->callback = std::move(callback);

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
    impl->stream = pw_stream_new(pipewire.core(), "wayfaraway RDP audio", props);
    if (!impl->stream)
        return nullptr;
    pw_stream_add_listener(impl->stream, &impl->stream_listener, &Impl::stream_events, impl.get());

    std::uint8_t pod_buffer[1024];
    const spa_pod* params[] = { build_audio_format(pod_buffer, sizeof(pod_buffer)) };

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
