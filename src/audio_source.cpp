#include "audio_source.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <pipewire/pipewire.h>

#include "audio_format.hpp"
#include "pipewire.hpp"
#include "pipewire_format.hpp"
#include "wlr.hpp"

namespace wfa {

namespace {

// Audio buffered beyond this is dropped (oldest first), bounding the delay
// a slow or bursty client can introduce.
constexpr std::size_t max_buffered_bytes = audio::rate * audio::bytes_per_frame / 4; // 250 ms
// Frames produced per cycle when the graph does not say how many it wants.
constexpr std::uint32_t default_quantum = 1024;

} // namespace

struct AudioSource::Impl {
    DemandCallback on_demand;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};
    bool recording = false;
    std::vector<std::byte> buffer;

    ~Impl()
    {
        if (stream)
            pw_stream_destroy(stream);
    }

    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* error)
    {
        auto* self = static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR)
            wlr_log(WLR_ERROR, "Audio: microphone stream failed: %s", error ? error : "unknown error");

        // Streaming means an application is linked to us and recording.
        const bool recording = state == PW_STREAM_STATE_STREAMING;
        if (recording == self->recording)
            return;
        self->recording = recording;
        wlr_log(WLR_DEBUG, "Audio: microphone %s", recording ? "in use" : "idle");
        if (!recording)
            self->buffer.clear();
        if (self->on_demand)
            self->on_demand(recording);
    }

    // Without PW_STREAM_FLAG_RT_PROCESS this runs on the main (Wayland) loop.
    static void on_process(void* data)
    {
        auto* self = static_cast<Impl*>(data);
        pw_buffer* b = pw_stream_dequeue_buffer(self->stream);
        if (!b)
            return;

        spa_data& d = b->buffer->datas[0];
        if (!d.data || !d.chunk) {
            pw_stream_queue_buffer(self->stream, b);
            return;
        }

        std::uint32_t frames = d.maxsize / audio::bytes_per_frame;
        frames = std::min(frames, b->requested ? static_cast<std::uint32_t>(b->requested) : default_quantum);
        const std::size_t bytes = std::size_t{ frames } * audio::bytes_per_frame;

        auto* out = static_cast<std::byte*>(d.data);
        const std::size_t available = std::min(bytes, self->buffer.size());
        std::memcpy(out, self->buffer.data(), available);
        std::memset(out + available, 0, bytes - available); // underrun: silence
        self->buffer.erase(self->buffer.begin(), self->buffer.begin() + static_cast<std::ptrdiff_t>(available));

        d.chunk->offset = 0;
        d.chunk->stride = audio::bytes_per_frame;
        d.chunk->size = static_cast<std::uint32_t>(bytes);
        pw_stream_queue_buffer(self->stream, b);
    }

    static constexpr pw_stream_events stream_events = [] {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &Impl::on_state_changed;
        events.process = &Impl::on_process;
        return events;
    }();
};

std::unique_ptr<AudioSource> AudioSource::create(PipeWire& pipewire, DemandCallback on_demand)
{
    auto impl = std::make_unique<Impl>();
    impl->on_demand = std::move(on_demand);

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, "wayfaraway-rdp-mic",
        PW_KEY_NODE_DESCRIPTION, "Remote desktop microphone",
        PW_KEY_NODE_WANT_DRIVER, "true",
        // Preferred as the default input, like the sink is for output.
        PW_KEY_PRIORITY_SESSION, "2000",
        "audio.position", "FL,FR",
        nullptr);
    impl->stream = pw_stream_new(pipewire.core(), "wayfaraway RDP microphone", props);
    if (!impl->stream)
        return nullptr;
    pw_stream_add_listener(impl->stream, &impl->stream_listener, &Impl::stream_events, impl.get());

    std::uint8_t pod_buffer[1024];
    const spa_pod* params[] = { build_audio_format(pod_buffer, sizeof(pod_buffer)) };

    if (pw_stream_connect(impl->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
        wlr_log(WLR_ERROR, "Audio: cannot create the PipeWire microphone source");
        return nullptr;
    }

    wlr_log(WLR_INFO, "Audio: PipeWire source 'wayfaraway-rdp-mic' ready");
    return std::make_unique<AudioSource>(std::move(impl));
}

AudioSource::AudioSource(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

AudioSource::~AudioSource() = default;

bool AudioSource::recording() const
{
    return m_impl->recording;
}

void AudioSource::push(std::span<const std::byte> samples)
{
    if (!m_impl->recording)
        return;
    auto& buffer = m_impl->buffer;
    buffer.insert(buffer.end(), samples.begin(), samples.end());
    if (buffer.size() > max_buffered_bytes) {
        // Keep the newest audio, aligned to whole frames.
        std::size_t excess = buffer.size() - max_buffered_bytes;
        excess += (audio::bytes_per_frame - excess % audio::bytes_per_frame) % audio::bytes_per_frame;
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(excess));
    }
}

void AudioSource::flush()
{
    m_impl->buffer.clear();
}

} // namespace wfa
