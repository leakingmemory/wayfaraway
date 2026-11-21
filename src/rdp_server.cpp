#include "rdp_server.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>

#include <linux/input-event-codes.h>

#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/constants.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/display.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <winpr/input.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/wtsapi.h>

#include "audio_sink.hpp"
#include "audio_source.hpp"
#include "clipboard.hpp"
#include "pipewire.hpp"
#include "rdp_audio.hpp"
#include "rdp_display.hpp"
#include "rdp_microphone.hpp"
#include "tls_certificate.hpp"

namespace wfa {

namespace {

// RemoteFX frames can be large; clients advertising less than this cannot
// receive a full-screen update in one fragmented PDU and fall back to NSCodec.
constexpr UINT32 min_rfx_multifrag_size = 0x3F0000;

// Beyond this many rectangles, encoding the bounding box is cheaper.
constexpr int max_rects = 64;

// Room left in each PDU for surface-bits and fast-path headers.
constexpr UINT32 pdu_header_margin = 1024;
// Assumed PDU limit for clients that do not advertise multifragment support.
constexpr UINT32 min_pdu_size = 16 * 1024;

// Frames we allow in flight before waiting for the client to acknowledge one.
// The client's advertised maximum is honoured, but capped so a slow link does
// not build up seconds of stale frames.
constexpr UINT32 default_frames_in_flight = 2;
constexpr UINT32 max_frames_in_flight = 4;
// If no acknowledgement arrives for this long, stop waiting for it.
constexpr int ack_timeout_ms = 1000;

enum class Codec { RemoteFx, NSCodec };

const char* codec_name(Codec codec)
{
    return codec == Codec::RemoteFx ? "RemoteFX" : "NSCodec";
}

std::vector<wl_event_source*> watch_handles(wl_event_loop* loop, HANDLE* handles, DWORD count,
    wl_event_loop_fd_func_t callback, void* data)
{
    std::vector<wl_event_source*> sources;
    for (DWORD i = 0; i < count; ++i) {
        int fd = GetEventFileDescriptor(handles[i]);
        if (fd < 0)
            continue;
        if (auto* source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, callback, data))
            sources.push_back(source);
    }
    return sources;
}

void remove_sources(std::vector<wl_event_source*>& sources)
{
    for (auto* source : sources)
        wl_event_source_remove(source);
    sources.clear();
}

} // namespace

// --- Peer --------------------------------------------------------------------

class RdpPeer {
public:
    RdpPeer(RdpServer& server, freerdp_peer* client)
        : m_server(server)
        , m_client(client)
    {
        pixman_region32_init(&m_pending);
    }

    ~RdpPeer()
    {
        detach();
        pixman_region32_fini(&m_pending);
        if (m_vcm)
            WTSCloseServer(m_vcm);
        if (m_client) {
            m_client->Disconnect(m_client);
            freerdp_peer_context_free(m_client);
            freerdp_peer_free(m_client);
        }
        if (m_rfx)
            rfx_context_free(m_rfx);
        if (m_nsc)
            nsc_context_free(m_nsc);
        if (m_stream)
            Stream_Free(m_stream, TRUE);
    }

    RdpPeer(const RdpPeer&) = delete;
    RdpPeer& operator=(const RdpPeer&) = delete;

    const char* hostname() const { return m_client ? m_client->hostname : "?"; }

    bool init();

    // On failure FreeRDP frees the peer itself, so we must not.
    void release_client()
    {
        if (m_client)
            freerdp_peer_context_free(m_client);
        m_client = nullptr;
    }

    // Stops processing events; the object is destroyed later.
    void detach()
    {
        // Gives up the client's clipboard offer on the seat right away.
        m_clipboard.reset();
        m_audio.reset();
        m_microphone.reset(); // joins FreeRDP's audin thread
        m_display.reset(); // joins FreeRDP's disp thread
        m_closed = true;
        remove_sources(m_sources);
        if (m_ack_timer) {
            wl_event_source_remove(m_ack_timer);
            m_ack_timer = nullptr;
        }
    }

    // Queues `region` for sending; it goes out now unless the client is
    // still behind on acknowledging earlier frames.
    void send_region(const pixman_region32_t* region);
    void send_full_frame();
    void output_resized();
    void lock_changed(bool locked)
    {
        if (m_clipboard)
            m_clipboard->set_locked(locked);
        if (m_audio)
            m_audio->set_locked(locked);
        update_microphone();
    }

    // Mirrors our Caps/Num Lock state onto the client's keyboard LEDs.
    void set_keyboard_leds(UINT32 server_flags)
    {
        // Until the client has reported its own state, it wins: pushing ours
        // first would overwrite it. Only the locks we actually sync are ours;
        // Scroll Lock and Kana stay as the client reported them, since on
        // Windows clients this PDU really toggles the local keyboard.
        if (!m_active || !m_client_leds)
            return;
        constexpr UINT32 ours = KBD_SYNC_CAPS_LOCK | KBD_SYNC_NUM_LOCK;
        const UINT32 wanted = (*m_client_leds & ~ours) | (server_flags & ours);
        if (wanted == *m_client_leds)
            return;
        m_client_leds = wanted;
        (void)update()->SetKeyboardIndicators(m_client->context, static_cast<UINT16>(wanted));
    }

    void set_microphone_demand(bool wanted)
    {
        m_microphone_demand = wanted;
        update_microphone();
    }

    void push_audio(std::span<const std::byte> samples)
    {
        if (m_audio)
            m_audio->push(samples);
    }

private:
    // The client's microphone is open only while unlocked and recording.
    void update_microphone()
    {
        if (m_microphone)
            m_microphone->set_open(m_dynamic_ready && m_microphone_demand && !compositor().locked());
    }

    // Dynamic channels (audin, disp) can only be opened once drdynvc has
    // finished negotiating, which happens some time after activation.
    void poll_dynamic_channels()
    {
        if (m_closed || m_dynamic_ready || !m_vcm
            || WTSVirtualChannelManagerGetDrdynvcState(m_vcm) != DRDYNVC_STATE_READY)
            return;
        m_dynamic_ready = true;
        if (m_display)
            m_display->open();
        update_microphone();
    }

    // Tells the client about our monitor layout (in its coordinates, where
    // the primary monitor's top-left is the origin).
    void store_monitor_layout(const std::vector<Monitor>& monitors);

    bool choose_codec();
    UINT32 frames_in_flight() const { return m_frame_id - m_last_acked; }
    bool throttled() const { return m_ack_throttle && frames_in_flight() >= m_max_in_flight; }
    void flush();
    void transmit(const pixman_region32_t* region);
    void arm_ack_timer();
    static int on_ack_timeout(void* data);
    void encode_rfx(const pixman_region32_t* region);
    void encode_nsc(const pixman_region32_t* region);
    void send_surface_bits(const pixman_box32_t& box, UINT16 codec_id);

    struct Context {
        rdpContext base; // must stay first
        RdpPeer* peer;
    };

    static RdpPeer* from(rdpContext* context) { return reinterpret_cast<Context*>(context)->peer; }
    static RdpPeer* from(freerdp_peer* client) { return from(client->context); }

    rdpSettings* settings() const { return m_client->context->settings; }
    rdpUpdate* update() const { return m_client->context->update; }
    Server& compositor() const { return m_server.server(); }

    static int on_fd(int fd, uint32_t mask, void* data);
    static int on_vcm(int fd, uint32_t mask, void* data);

    static BOOL on_post_connect(freerdp_peer* client);
    static BOOL on_activate(freerdp_peer* client);
    static BOOL on_synchronize(rdpInput* input, UINT32 flags);
    static BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code);
    static BOOL on_unicode_keyboard(rdpInput* input, UINT16 flags, UINT16 code);
    static BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
    static BOOL on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
    static BOOL on_refresh_rect(rdpContext* context, BYTE count, const RECTANGLE_16* areas);
    static BOOL on_suppress_output(rdpContext* context, BYTE allow, const RECTANGLE_16* area);
    static BOOL on_frame_acknowledge(rdpContext* context, UINT32 frame_id);

    RdpServer& m_server;
    freerdp_peer* m_client;
    std::vector<wl_event_source*> m_sources;
    HANDLE m_vcm = nullptr;
    std::unique_ptr<Clipboard> m_clipboard;
    std::unique_ptr<RdpAudio> m_audio;
    std::unique_ptr<RdpMicrophone> m_microphone;
    bool m_microphone_demand = false;
    std::unique_ptr<RdpDisplayControl> m_display;
    bool m_dynamic_ready = false;
    bool m_closed = false;
    // The layout the client was last told about.
    std::vector<Monitor> m_sent_monitors;
    // Lock-key state the client is known to have (KBD_SYNC_* flags).
    std::optional<UINT32> m_client_leds;

    Codec m_codec = Codec::RemoteFx;
    RFX_CONTEXT* m_rfx = nullptr;
    NSC_CONTEXT* m_nsc = nullptr;
    wStream* m_stream = nullptr;
    // Largest encoded payload one surface-bits PDU may carry.
    UINT32 m_max_payload = 0;

    bool m_active = false;
    bool m_suppressed = false;
    UINT32 m_frame_id = 0;

    // Damage not yet sent. Pixels always come from the shadow buffer at send
    // time, so damage that piles up while throttled costs one encode.
    pixman_region32_t m_pending;

    bool m_ack_throttle = false;
    bool m_acks_seen = false;
    // Set once the client proves it advertises acknowledgement but never sends it.
    bool m_acks_broken = false;
    UINT32 m_max_in_flight = 0;
    UINT32 m_last_acked = 0;
    wl_event_source* m_ack_timer = nullptr;
    bool m_ack_timer_armed = false;
};

bool RdpPeer::init()
{
    m_client->ContextSize = sizeof(Context);
    if (!freerdp_peer_context_new(m_client)) {
        wlr_log(WLR_ERROR, "RDP: failed to create peer context");
        m_client = nullptr;
        return false;
    }
    reinterpret_cast<Context*>(m_client->context)->peer = this;

    const Config& config = compositor().config();
    rdpCertificate* certificate = freerdp_certificate_new_from_file(config.tls_cert.c_str());
    rdpPrivateKey* key = freerdp_key_new_from_file(config.tls_key.c_str());
    if (!certificate || !key) {
        wlr_log(WLR_ERROR, "RDP: cannot load TLS certificate '%s' / key '%s'", config.tls_cert.c_str(),
            config.tls_key.c_str());
        if (certificate)
            freerdp_certificate_free(certificate);
        if (key)
            freerdp_key_free(key);
        return false;
    }

    rdpSettings* s = settings();
    // Settings take ownership of the certificate and key.
    bool ok = freerdp_settings_set_pointer_len(s, FreeRDP_RdpServerCertificate, certificate, 1)
        && freerdp_settings_set_pointer_len(s, FreeRDP_RdpServerRsaKey, key, 1)
        // Authentication happens on our own lock screen, so the transport
        // only needs TLS; NLA would require a separate credential store.
        // Every NLA-like protocol must be off: FreeRDP checks RDSTLS and
        // HYBRID_EX (ExtSecurity) before plain NLA, and clients ask for them.
        && freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, FALSE)
        && freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, FALSE)
        && freerdp_settings_set_bool(s, FreeRDP_ExtSecurity, FALSE)
        && freerdp_settings_set_bool(s, FreeRDP_RdstlsSecurity, FALSE)
        && freerdp_settings_set_bool(s, FreeRDP_AadSecurity, FALSE)
        && freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, 32)
        && freerdp_settings_set_bool(s, FreeRDP_RemoteFxCodec, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_NSCodec, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_SupportGraphicsPipeline, FALSE)
        && freerdp_settings_set_bool(s, FreeRDP_SurfaceFrameMarkerEnabled, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_FrameMarkerCommandEnabled, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_RefreshRect, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_SuppressOutput, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_DesktopResize, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_HasExtendedMouseEvent, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_HasHorizontalWheel, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_UnicodeInput, FALSE)
        // Accept the client's monitor layout and let it change at runtime.
        && freerdp_settings_set_bool(s, FreeRDP_UseMultimon, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_SupportMonitorLayoutPdu, TRUE)
        && freerdp_settings_set_bool(s, FreeRDP_SupportDisplayControl, TRUE)
        && freerdp_settings_set_uint32(s, FreeRDP_MultifragMaxRequestSize, 0xFFFFFF);
    if (!ok) {
        wlr_log(WLR_ERROR, "RDP: failed to configure peer settings");
        return false;
    }

    m_client->PostConnect = &RdpPeer::on_post_connect;
    m_client->Activate = &RdpPeer::on_activate;

    rdpInput* input = m_client->context->input;
    input->SynchronizeEvent = &RdpPeer::on_synchronize;
    input->KeyboardEvent = &RdpPeer::on_keyboard;
    input->UnicodeKeyboardEvent = &RdpPeer::on_unicode_keyboard;
    input->MouseEvent = &RdpPeer::on_mouse;
    input->ExtendedMouseEvent = &RdpPeer::on_extended_mouse;

    update()->RefreshRect = &RdpPeer::on_refresh_rect;
    update()->SuppressOutput = &RdpPeer::on_suppress_output;
    update()->SurfaceFrameAcknowledge = &RdpPeer::on_frame_acknowledge;

    if (!m_client->Initialize(m_client)) {
        wlr_log(WLR_ERROR, "RDP: peer initialisation failed");
        return false;
    }
    // Initialize() resets FrameAcknowledge to 0, so it must be set after.
    if (!freerdp_settings_set_uint32(s, FreeRDP_FrameAcknowledge, default_frames_in_flight)) {
        wlr_log(WLR_ERROR, "RDP: failed to enable frame acknowledgement");
        return false;
    }

    m_stream = Stream_New(nullptr, 64 * 1024);
    if (!m_stream)
        return false;

    HANDLE handles[32];
    DWORD count = m_client->GetEventHandles(m_client, handles, std::size(handles));
    m_sources = watch_handles(compositor().event_loop(), handles, count, &RdpPeer::on_fd, this);
    if (m_sources.empty()) {
        wlr_log(WLR_ERROR, "RDP: peer exposes no pollable handles");
        return false;
    }

    // Virtual channels (clipboard). Routes the peer's channel data to it.
    m_vcm = WTSOpenServerA(reinterpret_cast<LPSTR>(m_client->context));
    if (!m_vcm) {
        wlr_log(WLR_ERROR, "RDP: cannot create the virtual channel manager");
        return false;
    }
    HANDLE vcm_handle = WTSVirtualChannelManagerGetEventHandle(m_vcm);
    auto vcm_sources = watch_handles(compositor().event_loop(), &vcm_handle, 1, &RdpPeer::on_vcm, this);
    m_sources.insert(m_sources.end(), vcm_sources.begin(), vcm_sources.end());
    return true;
}

int RdpPeer::on_vcm(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<RdpPeer*>(data);
    if (!WTSVirtualChannelManagerCheckFileDescriptor(self->m_vcm)) {
        wlr_log(WLR_INFO, "RDP: %s virtual channel failure", self->hostname());
        self->m_server.peer_closed(self);
        return 0;
    }
    self->poll_dynamic_channels();
    return 0;
}

int RdpPeer::on_fd(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<RdpPeer*>(data);
    if (!self->m_client->CheckFileDescriptor(self->m_client)) {
        wlr_log(WLR_INFO, "RDP: %s disconnected", self->hostname());
        self->m_server.peer_closed(self);
        return 0;
    }
    // drdynvc negotiation arrives as ordinary channel data.
    self->poll_dynamic_channels();
    return 0;
}

BOOL RdpPeer::on_post_connect(freerdp_peer* client)
{
    RdpPeer* self = from(client);
    rdpSettings* s = self->settings();

    if (!self->choose_codec())
        return FALSE;

    const auto width = static_cast<int>(freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth));
    const auto height = static_cast<int>(freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight));

    // The client's monitors (with /multimon) or its window decide the
    // outputs. RDP monitor coordinates have the primary at the origin;
    // set_monitor_layout normalises them to desktop coordinates.
    std::vector<Monitor> requested;
    const UINT32 count = freerdp_settings_get_uint32(s, FreeRDP_MonitorCount);
    for (UINT32 i = 0; i < count; ++i) {
        const auto* m = static_cast<const rdpMonitor*>(freerdp_settings_get_pointer_array(s, FreeRDP_MonitorDefArray, i));
        if (m)
            requested.push_back({ m->x, m->y, m->width, m->height, m->is_primary != 0 });
    }
    if (requested.empty())
        requested.push_back({ 0, 0, width, height, true });

    wlr_log(WLR_INFO, "RDP: %s connected, desktop %dx%d on %zu monitor(s), %s", self->hostname(), width, height,
        requested.size(), codec_name(self->m_codec));

    const std::vector<Monitor> applied = self->compositor().set_monitor_layout(std::move(requested));
    self->store_monitor_layout(applied);
    return TRUE;
}

void RdpPeer::store_monitor_layout(const std::vector<Monitor>& monitors)
{
    rdpSettings* s = settings();
    const Monitor& primary = monitors.front();

    (void)freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth, static_cast<UINT32>(compositor().desktop_width()));
    (void)freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight, static_cast<UINT32>(compositor().desktop_height()));

    if (freerdp_settings_set_pointer_len(s, FreeRDP_MonitorDefArray, nullptr, monitors.size())) {
        for (std::size_t i = 0; i < monitors.size(); ++i) {
            const Monitor& m = monitors[i];
            rdpMonitor monitor{};
            monitor.x = m.x - primary.x;
            monitor.y = m.y - primary.y;
            monitor.width = m.width;
            monitor.height = m.height;
            monitor.is_primary = m.primary ? 1 : 0;
            (void)freerdp_settings_set_pointer_array(s, FreeRDP_MonitorDefArray, i, &monitor);
        }
        (void)freerdp_settings_set_uint32(s, FreeRDP_MonitorCount, static_cast<UINT32>(monitors.size()));
    }
    m_sent_monitors = monitors;
}

BOOL RdpPeer::on_activate(freerdp_peer* client)
{
    RdpPeer* self = from(client);
    rdpSettings* s = self->settings();

    const auto width = freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth);
    const auto height = freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight);
    if (self->m_rfx && !rfx_context_reset(self->m_rfx, width, height))
        return FALSE;
    if (self->m_nsc && !nsc_context_reset(self->m_nsc, width, height))
        return FALSE;

    if (!self->m_clipboard && WTSVirtualChannelManagerIsChannelJoined(self->m_vcm, CLIPRDR_SVC_CHANNEL_NAME)) {
        self->m_clipboard = Clipboard::create(self->compositor(), self->m_vcm);
        if (!self->m_clipboard)
            wlr_log(WLR_ERROR, "RDP: clipboard unavailable for %s", self->hostname());
    }
    if (!self->m_audio && WTSVirtualChannelManagerIsChannelJoined(self->m_vcm, RDPSND_CHANNEL_NAME)) {
        self->m_audio = RdpAudio::create(self->compositor().event_loop(), self->m_vcm, self->compositor().locked());
        if (!self->m_audio)
            wlr_log(WLR_ERROR, "RDP: audio unavailable for %s", self->hostname());
    }
    // audin is a dynamic channel, carried by drdynvc.
    if (!self->m_microphone && WTSVirtualChannelManagerIsChannelJoined(self->m_vcm, DRDYNVC_SVC_CHANNEL_NAME)) {
        RdpServer& server = self->m_server;
        self->m_microphone = RdpMicrophone::create(self->compositor().event_loop(), self->m_vcm, client->context,
            [&server](std::span<const std::byte> samples) { server.deliver_microphone(samples); });
        if (!self->m_microphone)
            wlr_log(WLR_ERROR, "RDP: microphone unavailable for %s", self->hostname());
    }
    self->set_microphone_demand(self->m_server.microphone_wanted());
    if (!self->m_display && WTSVirtualChannelManagerIsChannelJoined(self->m_vcm, DRDYNVC_SVC_CHANNEL_NAME)) {
        self->m_display = RdpDisplayControl::create(self->compositor().event_loop(), self->m_vcm, client->context,
            [self](std::vector<Monitor> monitors) {
                // Resizes and rearranges the outputs; output_resized() then
                // tells the client.
                self->compositor().set_monitor_layout(std::move(monitors));
            });
        if (!self->m_display)
            wlr_log(WLR_ERROR, "RDP: display control unavailable for %s", self->hostname());
    }
    self->poll_dynamic_channels();

    // The cursor is composited into the frames; hide the client's own.
    POINTER_SYSTEM_UPDATE pointer{};
    pointer.type = SYSPTR_NULL;
    (void)self->update()->pointer->PointerSystem(client->context, &pointer);

    // Frame acknowledgement needs frame markers to number the frames.
    // After negotiation, FrameAcknowledge holds the client's maximum.
    const UINT32 client_max = freerdp_settings_get_uint32(s, FreeRDP_FrameAcknowledge);
    const bool markers = freerdp_settings_get_bool(s, FreeRDP_SurfaceFrameMarkerEnabled);
    self->m_ack_throttle = client_max > 0 && markers && !self->m_acks_broken;
    self->m_max_in_flight = std::clamp(client_max, UINT32{ 1 }, max_frames_in_flight);
    if (self->m_ack_throttle && self->m_frame_id == 0)
        wlr_log(WLR_INFO, "RDP: frame acknowledgement on, up to %u frames in flight", self->m_max_in_flight);
    // Frames sent before (re)activation will never be acknowledged.
    self->m_last_acked = self->m_frame_id;

    self->m_active = true;
    self->m_suppressed = false;
    self->send_full_frame();
    return TRUE;
}

void RdpPeer::output_resized()
{
    if (!m_active)
        return;

    rdpSettings* s = settings();
    const auto width = static_cast<UINT32>(m_server.width());
    const auto height = static_cast<UINT32>(m_server.height());
    const std::vector<Monitor>& monitors = compositor().monitors();
    if (freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth) == width
        && freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight) == height && monitors == m_sent_monitors)
        return;

    if (!freerdp_settings_get_bool(s, FreeRDP_DesktopResize)) {
        wlr_log(WLR_ERROR, "RDP: output resized but client cannot follow; disconnecting");
        m_server.peer_closed(this);
        return;
    }

    store_monitor_layout(monitors);

    // Say how the new desktop splits into monitors before the resize, so
    // the client lays it out correctly on reactivation.
    if (freerdp_settings_get_bool(s, FreeRDP_SupportMonitorLayoutPdu)) {
        const Monitor& primary = monitors.front();
        std::vector<MONITOR_DEF> defs;
        for (const Monitor& m : monitors) {
            defs.push_back(MONITOR_DEF{
                .left = m.x - primary.x,
                .top = m.y - primary.y,
                .right = m.x - primary.x + m.width - 1,
                .bottom = m.y - primary.y + m.height - 1,
                .flags = m.primary ? MONITOR_PRIMARY : 0u,
            });
        }
        if (!freerdp_display_send_monitor_layout(m_client->context, static_cast<UINT32>(defs.size()), defs.data()))
            wlr_log(WLR_ERROR, "RDP: failed to send the monitor layout");
    }

    // The client re-runs activation after a desktop resize.
    m_active = false;
    (void)update()->DesktopResize(m_client->context);
}

void RdpPeer::send_full_frame()
{
    pixman_region32_t full;
    pixman_region32_init_rect(&full, 0, 0, static_cast<unsigned>(m_server.width()),
        static_cast<unsigned>(m_server.height()));
    send_region(&full);
    pixman_region32_fini(&full);
}

void RdpPeer::send_region(const pixman_region32_t* region)
{
    // Inactive peers get a full frame on (re)activation anyway.
    if (!m_active)
        return;
    pixman_region32_union(&m_pending, &m_pending, region);
    flush();
}

void RdpPeer::flush()
{
    if (!m_active || m_suppressed || !m_server.shadow() || !pixman_region32_not_empty(&m_pending))
        return;

    rdpSettings* s = settings();
    const int width = m_server.width();
    const int height = m_server.height();
    if (freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth) != static_cast<UINT32>(width)
        || freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight) != static_cast<UINT32>(height))
        return; // waiting for a desktop resize round-trip

    if (throttled()) {
        arm_ack_timer();
        return;
    }

    pixman_region32_t clipped;
    pixman_region32_init(&clipped);
    pixman_region32_intersect_rect(&clipped, &m_pending, 0, 0, static_cast<unsigned>(width),
        static_cast<unsigned>(height));
    pixman_region32_clear(&m_pending);
    if (pixman_region32_not_empty(&clipped))
        transmit(&clipped);
    pixman_region32_fini(&clipped);
}

void RdpPeer::transmit(const pixman_region32_t* region)
{
    const bool markers = freerdp_settings_get_bool(settings(), FreeRDP_SurfaceFrameMarkerEnabled);
    rdpContext* context = m_client->context;
    SURFACE_FRAME_MARKER marker{ SURFACECMD_FRAMEACTION_BEGIN, ++m_frame_id };
    if (markers)
        (void)update()->SurfaceFrameMarker(context, &marker);

    if (m_codec == Codec::RemoteFx)
        encode_rfx(region);
    else
        encode_nsc(region);

    if (markers) {
        marker.frameAction = SURFACECMD_FRAMEACTION_END;
        (void)update()->SurfaceFrameMarker(context, &marker);
    }
}

void RdpPeer::arm_ack_timer()
{
    // Armed once per stall: re-arming on every throttled frame would keep
    // pushing the timeout back while damage keeps coming.
    if (m_ack_timer_armed)
        return;
    if (!m_ack_timer) {
        m_ack_timer = wl_event_loop_add_timer(compositor().event_loop(), &RdpPeer::on_ack_timeout, this);
        if (!m_ack_timer)
            return;
    }
    wl_event_source_timer_update(m_ack_timer, ack_timeout_ms);
    m_ack_timer_armed = true;
}

int RdpPeer::on_ack_timeout(void* data)
{
    auto* self = static_cast<RdpPeer*>(data);
    self->m_ack_timer_armed = false;
    if (!self->throttled())
        return 0;

    if (!self->m_acks_seen) {
        // The client advertised the capability but never uses it.
        wlr_log(WLR_INFO, "RDP: %s never acknowledged a frame; disabling frame acknowledgement", self->hostname());
        self->m_ack_throttle = false;
        self->m_acks_broken = true;
    } else {
        wlr_log(WLR_DEBUG, "RDP: frame acknowledgement timed out with %u in flight", self->frames_in_flight());
        self->m_last_acked = self->m_frame_id;
    }
    self->flush();
    return 0;
}

BOOL RdpPeer::on_frame_acknowledge(rdpContext* context, UINT32 frame_id)
{
    RdpPeer* self = from(context);
    self->m_acks_seen = true;
    // Ignore stale or bogus ids; the comparison is wrap-safe.
    const auto ahead = static_cast<INT32>(frame_id - self->m_last_acked);
    const auto sent = static_cast<INT32>(self->m_frame_id - self->m_last_acked);
    if (ahead > 0 && ahead <= sent)
        self->m_last_acked = frame_id;

    if (!self->throttled() && self->m_ack_timer_armed) {
        wl_event_source_timer_update(self->m_ack_timer, 0);
        self->m_ack_timer_armed = false;
    }
    self->flush();
    return TRUE;
}

bool RdpPeer::choose_codec()
{
    rdpSettings* s = settings();
    const UINT32 multifrag = freerdp_settings_get_uint32(s, FreeRDP_MultifragMaxRequestSize);
    const bool rfx = freerdp_settings_get_bool(s, FreeRDP_RemoteFxCodec) && multifrag >= min_rfx_multifrag_size;
    const bool nsc = freerdp_settings_get_bool(s, FreeRDP_NSCodec);

    if (rfx) {
        m_codec = Codec::RemoteFx;
        m_rfx = rfx_context_new(TRUE);
        if (!m_rfx)
            return false;
        rfx_context_set_pixel_format(m_rfx, PIXEL_FORMAT_BGRX32);
        return rfx_context_set_mode(m_rfx, RLGR3);
    }

    if (nsc) {
        m_codec = Codec::NSCodec;
        m_max_payload = std::max(multifrag, min_pdu_size) - pdu_header_margin;
        m_nsc = nsc_context_new();
        if (!m_nsc)
            return false;
        // Use the quality settings the client negotiated.
        return nsc_context_set_parameters(m_nsc, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRX32)
            && nsc_context_set_parameters(m_nsc, NSC_COLOR_LOSS_LEVEL,
                freerdp_settings_get_uint32(s, FreeRDP_NSCodecColorLossLevel))
            && nsc_context_set_parameters(m_nsc, NSC_ALLOW_SUBSAMPLING,
                freerdp_settings_get_bool(s, FreeRDP_NSCodecAllowSubsampling))
            && nsc_context_set_parameters(m_nsc, NSC_DYNAMIC_COLOR_FIDELITY,
                freerdp_settings_get_bool(s, FreeRDP_NSCodecAllowDynamicColorFidelity));
    }

    wlr_log(WLR_ERROR, "RDP: client %s supports neither RemoteFX (multifrag %u) nor NSCodec; refusing",
        hostname(), multifrag);
    return false;
}

void RdpPeer::encode_rfx(const pixman_region32_t* region)
{
    const pixman_box32_t extents = *pixman_region32_extents(region);
    int count = 0;
    const pixman_box32_t* boxes = pixman_region32_rectangles(region, &count);

    std::vector<RFX_RECT> rects;
    auto add_rect = [&](const pixman_box32_t& box) {
        rects.push_back(RFX_RECT{
            .x = static_cast<UINT16>(box.x1 - extents.x1),
            .y = static_cast<UINT16>(box.y1 - extents.y1),
            .width = static_cast<UINT16>(box.x2 - box.x1),
            .height = static_cast<UINT16>(box.y2 - box.y1),
        });
    };
    if (count > max_rects) {
        add_rect(extents);
    } else {
        for (int i = 0; i < count; ++i)
            add_rect(boxes[i]);
    }

    const std::size_t stride = m_server.stride();
    const auto* origin = reinterpret_cast<const BYTE*>(m_server.shadow() + extents.y1 * stride + extents.x1 * 4);

    Stream_SetPosition(m_stream, 0);
    if (!rfx_compose_message(m_rfx, m_stream, rects.data(), rects.size(), origin,
            static_cast<UINT32>(extents.x2 - extents.x1), static_cast<UINT32>(extents.y2 - extents.y1),
            static_cast<UINT32>(stride))) {
        wlr_log(WLR_ERROR, "RDP: RemoteFX encoding failed");
        return;
    }
    send_surface_bits(extents, static_cast<UINT16>(freerdp_settings_get_uint32(settings(), FreeRDP_RemoteFxCodecId)));
}

void RdpPeer::encode_nsc(const pixman_region32_t* region)
{
    // NSCodec carries one rectangle per message and has no tiling of its
    // own, so split each box into tiles whose worst-case encoding (four
    // planes, padded to 8x2) fits in a single PDU.
    const auto codec_id = static_cast<UINT16>(freerdp_settings_get_uint32(settings(), FreeRDP_NSCodecId));
    const std::size_t stride = m_server.stride();
    const auto worst_case = [](int w, int h) {
        return static_cast<UINT64>((w + 7) & ~7) * static_cast<UINT64>((h + 1) & ~1) * 4;
    };

    int count = 0;
    const pixman_box32_t* boxes = pixman_region32_rectangles(region, &count);
    for (int i = 0; i < count; ++i) {
        const pixman_box32_t& box = boxes[i];
        const int box_w = box.x2 - box.x1;

        // Widest tile that still fits two rows, then as many rows as fit.
        int tile_w = box_w;
        while (tile_w > 8 && worst_case(tile_w, 2) > m_max_payload)
            tile_w = (tile_w + 1) / 2;
        int tile_h = 2;
        while (tile_h < box.y2 - box.y1 && worst_case(tile_w, tile_h * 2) <= m_max_payload)
            tile_h *= 2;

        for (int y = box.y1; y < box.y2; y += tile_h) {
            for (int x = box.x1; x < box.x2; x += tile_w) {
                const pixman_box32_t tile{ x, y, std::min(x + tile_w, box.x2), std::min(y + tile_h, box.y2) };
                const auto* origin = reinterpret_cast<const BYTE*>(m_server.shadow() + y * stride + x * 4);

                Stream_SetPosition(m_stream, 0);
                if (!nsc_compose_message(m_nsc, m_stream, origin, static_cast<UINT32>(tile.x2 - tile.x1),
                        static_cast<UINT32>(tile.y2 - tile.y1), static_cast<UINT32>(stride))) {
                    wlr_log(WLR_ERROR, "RDP: NSCodec encoding failed");
                    return;
                }
                send_surface_bits(tile, codec_id);
            }
        }
    }
}

void RdpPeer::send_surface_bits(const pixman_box32_t& box, UINT16 codec_id)
{
    SURFACE_BITS_COMMAND cmd{};
    cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
    cmd.destLeft = static_cast<UINT32>(box.x1);
    cmd.destTop = static_cast<UINT32>(box.y1);
    cmd.destRight = static_cast<UINT32>(box.x2);
    cmd.destBottom = static_cast<UINT32>(box.y2);
    cmd.bmp.bpp = 32;
    cmd.bmp.codecID = codec_id;
    cmd.bmp.width = static_cast<UINT16>(box.x2 - box.x1);
    cmd.bmp.height = static_cast<UINT16>(box.y2 - box.y1);
    cmd.bmp.bitmapDataLength = static_cast<UINT32>(Stream_GetPosition(m_stream));
    cmd.bmp.bitmapData = Stream_Buffer(m_stream);
    (void)update()->SurfaceBits(m_client->context, &cmd);
}

BOOL RdpPeer::on_synchronize(rdpInput* input, UINT32 flags)
{
    // Sent on connect and whenever the client window regains focus, so lock
    // keys toggled elsewhere on the client do not leave us out of step.
    RdpPeer* self = from(input->context);
    // Recorded first, so the LED update our own change triggers is no echo.
    self->m_client_leds = flags & (KBD_SYNC_SCROLL_LOCK | KBD_SYNC_NUM_LOCK | KBD_SYNC_CAPS_LOCK | KBD_SYNC_KANA_LOCK);
    self->compositor().sync_lock_keys(flags & KBD_SYNC_CAPS_LOCK, flags & KBD_SYNC_NUM_LOCK);
    return TRUE;
}

BOOL RdpPeer::on_keyboard(rdpInput* input, UINT16 flags, UINT8 code)
{
    RdpPeer* self = from(input->context);
    if (flags & KBD_FLAGS_EXTENDED1)
        return TRUE; // Pause/Break prefix; not mapped yet.

    DWORD scancode = code;
    if (flags & KBD_FLAGS_EXTENDED)
        scancode |= KBDEXT;

    const DWORD vk = GetVirtualKeyCodeFromVirtualScanCode(scancode, WINPR_KBD_TYPE_IBM_ENHANCED);
    DWORD keycode = GetKeycodeFromVirtualKeyCode(vk, WINPR_KEYCODE_TYPE_EVDEV);
    if (keycode == 0)
        keycode = GetKeycodeFromVirtualKeyCode(vk & 0xFF, WINPR_KEYCODE_TYPE_EVDEV);
    // WinPR returns X11-style evdev codes, offset by 8.
    if (keycode < 8)
        return TRUE;

    self->compositor().inject_key(keycode - 8, !(flags & KBD_FLAGS_RELEASE));
    return TRUE;
}

BOOL RdpPeer::on_unicode_keyboard(rdpInput* input, UINT16 flags, UINT16 code)
{
    wlr_log(WLR_DEBUG, "RDP: ignoring unicode key event U+%04X", code);
    return TRUE;
}

BOOL RdpPeer::on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
    Server& server = from(input->context)->compositor();

    if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) {
        // 9-bit two's complement rotation, 120 units per notch, positive
        // meaning away from the user / to the right.
        int rotation = flags & WheelRotationMask;
        if (rotation & 0x100)
            rotation -= 0x200;
        if (flags & PTR_FLAGS_WHEEL)
            server.inject_pointer_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, -rotation / 120.0 * 15.0, -rotation);
        else
            server.inject_pointer_axis(WL_POINTER_AXIS_HORIZONTAL_SCROLL, rotation / 120.0 * 15.0, rotation);
        return TRUE;
    }

    server.inject_pointer_motion(x, y);

    const bool down = flags & PTR_FLAGS_DOWN;
    if (flags & PTR_FLAGS_BUTTON1)
        server.inject_pointer_button(BTN_LEFT, down);
    if (flags & PTR_FLAGS_BUTTON2)
        server.inject_pointer_button(BTN_RIGHT, down);
    if (flags & PTR_FLAGS_BUTTON3)
        server.inject_pointer_button(BTN_MIDDLE, down);
    return TRUE;
}

BOOL RdpPeer::on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
    Server& server = from(input->context)->compositor();
    server.inject_pointer_motion(x, y);

    const bool down = flags & PTR_XFLAGS_DOWN;
    if (flags & PTR_XFLAGS_BUTTON1)
        server.inject_pointer_button(BTN_SIDE, down);
    if (flags & PTR_XFLAGS_BUTTON2)
        server.inject_pointer_button(BTN_EXTRA, down);
    return TRUE;
}

BOOL RdpPeer::on_refresh_rect(rdpContext* context, BYTE count, const RECTANGLE_16* areas)
{
    RdpPeer* self = from(context);
    pixman_region32_t region;
    pixman_region32_init(&region);
    for (BYTE i = 0; i < count; ++i) {
        pixman_region32_union_rect(&region, &region, areas[i].left, areas[i].top,
            static_cast<unsigned>(areas[i].right - areas[i].left),
            static_cast<unsigned>(areas[i].bottom - areas[i].top));
    }
    self->send_region(&region);
    pixman_region32_fini(&region);
    return TRUE;
}

BOOL RdpPeer::on_suppress_output(rdpContext* context, BYTE allow, const RECTANGLE_16* area)
{
    RdpPeer* self = from(context);
    self->m_suppressed = !allow;
    if (allow)
        self->send_full_frame();
    return TRUE;
}

// --- Server ------------------------------------------------------------------

RdpServer::RdpServer(Server& server)
    : m_server(server)
{
}

RdpServer::~RdpServer()
{
    m_server.on_lock_changed = nullptr;
    m_server.on_keyboard_leds = nullptr;
    m_peer.reset();
#ifdef WAYFARAWAY_HAVE_PIPEWIRE
    m_microphone.reset();
    m_audio.reset();
    m_pipewire.reset();
#endif
    m_dead_peers.clear();
    if (m_reaper)
        wl_event_source_remove(m_reaper);
    remove_sources(m_listener_sources);
    if (m_listener) {
        m_listener->Close(m_listener);
        freerdp_listener_free(m_listener);
    }
}

bool RdpServer::listen()
{
    const Config& config = m_server.config();

    auto generated = ensure_tls_certificate(config.tls_cert, config.tls_key);
    if (!generated) {
        wlr_log(WLR_ERROR, "TLS: %s", generated.error().c_str());
        return false;
    }
    if (*generated)
        wlr_log(WLR_INFO, "TLS: generated a self-signed certificate in %s", config.tls_cert.c_str());

    // Clients show this on first connect; compare before trusting it.
    if (auto fingerprint = tls_certificate_fingerprint(config.tls_cert))
        wlr_log(WLR_INFO, "TLS: certificate SHA-256 fingerprint %s", fingerprint->c_str());
    else
        wlr_log(WLR_ERROR, "TLS: %s", fingerprint.error().c_str());

    // Required before any WTS virtual channel call.
    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
    m_server.on_keyboard_leds = [this](std::uint32_t flags) {
        if (m_peer)
            m_peer->set_keyboard_leds(flags);
    };
    m_server.on_lock_changed = [this](bool locked) {
#ifdef WAYFARAWAY_HAVE_PIPEWIRE
        // Do not play out microphone audio captured before the lock.
        if (locked && m_microphone)
            m_microphone->flush();
#endif
        if (m_peer)
            m_peer->lock_changed(locked);
    };

#ifdef WAYFARAWAY_HAVE_PIPEWIRE
    // Optional: without PipeWire the desktop simply has no audio.
    m_pipewire = PipeWire::connect(m_server.event_loop());
    if (m_pipewire) {
        m_audio = AudioSink::create(*m_pipewire, [this](std::span<const std::byte> samples) {
            if (m_peer)
                m_peer->push_audio(samples);
        });
        m_microphone = AudioSource::create(*m_pipewire, [this](bool recording) {
            if (m_peer)
                m_peer->set_microphone_demand(recording);
        });
    }
#endif

    m_listener = freerdp_listener_new();
    if (!m_listener)
        return false;
    m_listener->info = this;
    m_listener->PeerAccepted = [](freerdp_listener* listener, freerdp_peer* client) -> BOOL {
        return static_cast<RdpServer*>(listener->info)->accept(client) ? TRUE : FALSE;
    };

    if (!m_listener->Open(m_listener, config.rdp_address.c_str(), config.rdp_port)) {
        wlr_log(WLR_ERROR, "RDP: cannot listen on %s:%u", config.rdp_address.c_str(), config.rdp_port);
        return false;
    }

    HANDLE handles[32];
    DWORD count = m_listener->GetEventHandles(m_listener, handles, std::size(handles));
    m_listener_sources = watch_handles(m_server.event_loop(), handles, count, &RdpServer::on_listener_event, this);
    if (m_listener_sources.empty()) {
        wlr_log(WLR_ERROR, "RDP: listener exposes no pollable handles");
        return false;
    }

    wlr_log(WLR_INFO, "RDP: listening on %s:%u", config.rdp_address.c_str(), config.rdp_port);
    return true;
}

int RdpServer::on_listener_event(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<RdpServer*>(data);
    if (!self->m_listener->CheckFileDescriptor(self->m_listener))
        wlr_log(WLR_ERROR, "RDP: listener error");
    return 0;
}

bool RdpServer::accept(freerdp_peer* client)
{
    auto peer = std::make_unique<RdpPeer>(*this, client);
    if (!peer->init()) {
        peer->detach();
        peer->release_client();
        return false;
    }

    wlr_log(WLR_INFO, "RDP: accepted connection from %s", peer->hostname());

    // Taking over an existing session: drop the old viewer first.
    if (m_peer)
        peer_closed(m_peer.get());

    m_peer = std::move(peer);
    m_server.lock();
    return true;
}

bool RdpServer::microphone_wanted() const
{
#ifdef WAYFARAWAY_HAVE_PIPEWIRE
    return m_microphone && m_microphone->recording();
#else
    return false;
#endif
}

void RdpServer::deliver_microphone(std::span<const std::byte> samples)
{
#ifdef WAYFARAWAY_HAVE_PIPEWIRE
    if (m_microphone && !m_server.locked())
        m_microphone->push(samples);
#endif
}

void RdpServer::peer_closed(RdpPeer* peer)
{
    if (m_peer.get() != peer)
        return;

    peer->detach();
    m_dead_peers.push_back(std::move(m_peer));
    if (!m_reaper)
        m_reaper = wl_event_loop_add_idle(m_server.event_loop(), &RdpServer::on_reap, this);

    // Whatever happens next, the desktop is not left open.
    m_server.release_all_input();
    m_server.lock();
}

void RdpServer::on_reap(void* data)
{
    auto* self = static_cast<RdpServer*>(data);
    self->m_reaper = nullptr; // idle sources remove themselves
    self->m_dead_peers.clear();
}

void RdpServer::on_layout(int width, int height)
{
    // Start from black: gaps between monitors stay black, and every output
    // repaints itself in full right after this.
    m_width = width;
    m_height = height;
    m_stride = static_cast<std::size_t>(width) * 4;
    m_shadow.assign(m_stride * static_cast<std::size_t>(height), std::byte{});

    if (m_peer)
        m_peer->output_resized();
}

void RdpServer::on_frame(const std::byte* pixels, int width, int height, std::size_t stride, int x, int y,
    const pixman_region32_t* damage)
{
    // Damage within this output's buffer, clipped to the part of the desktop
    // the shadow covers.
    pixman_region32_t region;
    pixman_region32_init(&region);
    pixman_region32_intersect_rect(&region, damage, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height));
    pixman_region32_intersect_rect(&region, &region, -x, -y, static_cast<unsigned>(m_width),
        static_cast<unsigned>(m_height));

    int count = 0;
    const pixman_box32_t* boxes = pixman_region32_rectangles(&region, &count);
    for (int i = 0; i < count; ++i) {
        const std::size_t bytes = static_cast<std::size_t>(boxes[i].x2 - boxes[i].x1) * 4;
        for (int row = boxes[i].y1; row < boxes[i].y2; ++row) {
            std::memcpy(m_shadow.data() + static_cast<std::size_t>(y + row) * m_stride
                    + static_cast<std::size_t>(x + boxes[i].x1) * 4,
                pixels + static_cast<std::size_t>(row) * stride + static_cast<std::size_t>(boxes[i].x1) * 4, bytes);
        }
    }

    if (m_peer) {
        // To desktop coordinates.
        pixman_region32_translate(&region, x, y);
        m_peer->send_region(&region);
    }
    pixman_region32_fini(&region);
}

} // namespace wfa
