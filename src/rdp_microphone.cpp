#include "rdp_microphone.hpp"

#include <array>
#include <cstring>

#include <sys/eventfd.h>
#include <unistd.h>

#include <freerdp/codec/audio.h>
#include <freerdp/server/audin.h>
#include <winpr/stream.h>
#include <winpr/wtsapi.h>

#include "audio_format.hpp"
#include "wlr.hpp"

namespace wfa {

namespace {

// Samples not yet picked up by the event loop beyond this are dropped.
constexpr std::size_t max_pending_bytes = audio::rate * audio::bytes_per_frame / 2; // 500 ms

AUDIO_FORMAT pcm_format(UINT16 channels)
{
    AUDIO_FORMAT format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = channels;
    format.nSamplesPerSec = audio::rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<UINT16>(channels * audio::bytes_per_sample);
    format.nAvgBytesPerSec = audio::rate * format.nBlockAlign;
    return format;
}

// Only PCM at our own rate is negotiated, so no decoding or resampling is
// needed; mono is upmixed. Stereo is preferred.
const std::array<AUDIO_FORMAT, 2> server_formats = { pcm_format(2), pcm_format(1) };

} // namespace

// Runs on FreeRDP's audin thread.
struct RdpMicrophone::Callbacks {
    static UINT data(audin_server_context* context, const SNDIN_DATA* data)
    {
        auto* self = static_cast<RdpMicrophone*>(context->userdata);
        const AUDIO_FORMAT* format = audin_server_get_negotiated_format(context);
        if (!format || format->wFormatTag != WAVE_FORMAT_PCM || format->wBitsPerSample != 16
            || format->nSamplesPerSec != audio::rate || (format->nChannels != 1 && format->nChannels != 2))
            return CHANNEL_RC_OK;

        const auto* bytes = reinterpret_cast<const std::byte*>(Stream_ConstPointer(data->Data));
        const std::size_t length = Stream_GetRemainingLength(data->Data);
        const std::size_t frame = format->nChannels * audio::bytes_per_sample;
        const std::size_t frames = length / frame;

        {
            std::scoped_lock lock(self->m_mutex);
            auto& pending = self->m_pending;
            if (pending.size() + frames * audio::bytes_per_frame > max_pending_bytes)
                pending.clear(); // the event loop is far behind; start over
            if (format->nChannels == 2) {
                pending.insert(pending.end(), bytes, bytes + frames * frame);
            } else {
                for (std::size_t i = 0; i < frames; ++i) {
                    const std::byte* sample = bytes + i * frame;
                    pending.insert(pending.end(), sample, sample + audio::bytes_per_sample); // left
                    pending.insert(pending.end(), sample, sample + audio::bytes_per_sample); // right
                }
            }
        }

        std::uint64_t one = 1;
        [[maybe_unused]] auto n = write(self->m_eventfd, &one, sizeof(one));
        return CHANNEL_RC_OK;
    }
};

std::unique_ptr<RdpMicrophone> RdpMicrophone::create(wl_event_loop* loop, void* vcm, rdpContext* context,
    Callback callback)
{
    auto microphone = std::make_unique<RdpMicrophone>(std::move(callback));
    if (!microphone->init(loop, vcm, context))
        return nullptr;
    return microphone;
}

RdpMicrophone::RdpMicrophone(Callback callback)
    : m_callback(std::move(callback))
{
}

bool RdpMicrophone::init(wl_event_loop* loop, void* vcm, rdpContext* context)
{
    m_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_eventfd < 0)
        return false;
    m_source = wl_event_loop_add_fd(loop, m_eventfd, WL_EVENT_READABLE, &RdpMicrophone::on_eventfd, this);
    if (!m_source)
        return false;

    m_context = audin_server_context_new(vcm);
    if (!m_context)
        return false;
    m_context->userdata = this;
    m_context->rdpcontext = context;
    // Version, format negotiation and the open reply use FreeRDP's defaults.
    m_context->Data = &Callbacks::data;
    if (!audin_server_set_formats(m_context, static_cast<SSIZE_T>(server_formats.size()), server_formats.data())) {
        wlr_log(WLR_ERROR, "Microphone: cannot set audin formats");
        return false;
    }
    return true;
}

RdpMicrophone::~RdpMicrophone()
{
    set_open(false); // joins the audin thread
    if (m_context)
        audin_server_context_free(m_context);
    if (m_source)
        wl_event_source_remove(m_source);
    if (m_eventfd >= 0)
        close(m_eventfd);
}

void RdpMicrophone::set_open(bool open)
{
    if (open == m_open || !m_context)
        return;

    if (open) {
        if (!m_context->Open(m_context)) {
            wlr_log(WLR_ERROR, "Microphone: cannot open the audin channel");
            return;
        }
        m_open = true;
        wlr_log(WLR_INFO, "Microphone: opened client microphone");
        return;
    }

    // Must not hold m_mutex here: Close waits for the audin thread, which
    // may be inside Callbacks::data.
    (void)m_context->Close(m_context);
    m_open = false;
    {
        std::scoped_lock lock(m_mutex);
        m_pending.clear();
    }
    wlr_log(WLR_INFO, "Microphone: closed client microphone");
}

int RdpMicrophone::on_eventfd(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<RdpMicrophone*>(data);
    std::uint64_t value = 0;
    [[maybe_unused]] auto n = read(fd, &value, sizeof(value));

    std::vector<std::byte> samples;
    {
        std::scoped_lock lock(self->m_mutex);
        samples.swap(self->m_pending);
    }
    if (self->m_open && !samples.empty() && self->m_callback)
        self->m_callback(samples);
    return 0;
}

} // namespace wfa
