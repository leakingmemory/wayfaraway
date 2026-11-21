#include "rdp_audio.hpp"

#include <freerdp/codec/audio.h>
#include <freerdp/server/rdpsnd.h>
#include <winpr/sysinfo.h>
#include <winpr/synch.h>
#include <winpr/wtsapi.h>

#include "audio_format.hpp"
#include "wlr.hpp"

namespace wfa {

namespace {

// Wave blocks are sized for this much audio; lower means less delay but
// more PDUs.
constexpr UINT32 latency_ms = 50;

AUDIO_FORMAT pcm_format()
{
    AUDIO_FORMAT format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = audio::channels;
    format.nSamplesPerSec = audio::rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = audio::bytes_per_frame;
    format.nAvgBytesPerSec = audio::rate * audio::bytes_per_frame;
    return format;
}

// FreeRDP keeps pointers to these for the context's lifetime.
AUDIO_FORMAT server_format = pcm_format();

} // namespace

std::unique_ptr<RdpAudio> RdpAudio::create(wl_event_loop* loop, void* vcm, bool locked)
{
    auto audio = std::make_unique<RdpAudio>(locked);
    if (!audio->start(loop, vcm))
        return nullptr;
    return audio;
}

RdpAudio::RdpAudio(bool locked)
    : m_locked(locked)
{
}

bool RdpAudio::start(wl_event_loop* loop, void* vcm)
{
    m_context = rdpsnd_server_context_new(vcm);
    if (!m_context)
        return false;

    m_context->data = this;
    m_context->use_dynamic_virtual_channel = FALSE;
    m_context->server_formats = &server_format;
    m_context->num_server_formats = 1;
    m_context->src_format = &server_format;
    m_context->latency = latency_ms;
    m_context->Activated = &RdpAudio::on_activated;

    // Not our own thread: messages are pumped from the Wayland event loop.
    if (m_context->Initialize(m_context, FALSE) != CHANNEL_RC_OK) {
        wlr_log(WLR_ERROR, "Audio: cannot start the rdpsnd channel");
        return false;
    }

    int fd = GetEventFileDescriptor(rdpsnd_server_get_event_handle(m_context));
    if (fd < 0)
        return false;
    m_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, &RdpAudio::on_channel_event, this);
    return m_source != nullptr;
}

RdpAudio::~RdpAudio()
{
    if (m_source)
        wl_event_source_remove(m_source);
    if (m_context) {
        if (m_context->Stop)
            m_context->Stop(m_context);
        rdpsnd_server_context_free(m_context);
    }
}

int RdpAudio::on_channel_event(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<RdpAudio*>(data);
    UINT status = rdpsnd_server_handle_messages(self->m_context);
    if (status != CHANNEL_RC_OK && status != ERROR_NO_DATA)
        wlr_log(WLR_DEBUG, "Audio: rdpsnd channel error 0x%08X", status);
    return 0;
}

void RdpAudio::on_activated(RdpsndServerContext* context)
{
    auto* self = static_cast<RdpAudio*>(context->data);
    for (UINT16 i = 0; i < context->num_client_formats; ++i) {
        if (audio_format_compatible(&server_format, &context->client_formats[i])) {
            if (context->SelectFormat(context, i) == CHANNEL_RC_OK) {
                self->m_format_selected = true;
                wlr_log(WLR_INFO, "Audio: streaming PCM %u Hz stereo to the client", audio::rate);
            }
            return;
        }
    }
    wlr_log(WLR_INFO, "Audio: client offers no PCM %u Hz stereo format; audio disabled", audio::rate);
}

void RdpAudio::push(std::span<const std::byte> samples)
{
    if (!m_format_selected || m_locked || samples.empty())
        return;
    const auto timestamp = static_cast<UINT16>(winpr_GetTickCount64() & 0xFFFF);
    (void)m_context->SendSamples(m_context, samples.data(), samples.size() / audio::bytes_per_frame, timestamp);
}

} // namespace wfa
