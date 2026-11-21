#include "clipboard.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#include <freerdp/channels/cliprdr.h>
#include <freerdp/server/cliprdr.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/user.h>
#include <winpr/wtsapi.h>

#include "server.hpp"

namespace wfa {

namespace {

// Largest clipboard payload accepted in either direction.
constexpr std::size_t max_clipboard_bytes = 16 * 1024 * 1024;
// How long a paste may wait for the RDP client or a Wayland source.
constexpr int transfer_timeout_ms = 5000;

// Text MIME types, in order of preference when reading from a Wayland source.
// All are treated as UTF-8.
constexpr std::array text_mime_types = {
    "text/plain;charset=utf-8",
    "UTF8_STRING",
    "text/plain",
    "TEXT",
    "STRING",
};

const char* preferred_text_mime(wlr_data_source* source)
{
    const auto* mimes = static_cast<char* const*>(source->mime_types.data);
    const std::size_t count = source->mime_types.size / sizeof(char*);
    for (const char* wanted : text_mime_types) {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(mimes[i], wanted) == 0)
                return wanted;
        }
    }
    return nullptr;
}

// Windows clipboard text uses CRLF line endings.
std::string to_crlf(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))
            out.push_back('\r');
        out.push_back(text[i]);
    }
    return out;
}

std::string from_crlf(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;
        out.push_back(text[i]);
    }
    return out;
}

bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Writes a buffer to a paste target's fd without blocking the event loop.
// Self-owned: deletes itself when done, so it can outlive the Clipboard.
class PipeWriter {
public:
    static void start(wl_event_loop* loop, int fd, std::shared_ptr<const std::string> data)
    {
        if (!set_nonblocking(fd)) {
            close(fd);
            return;
        }
        auto* writer = new PipeWriter(fd, std::move(data));
        writer->m_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE, &PipeWriter::on_writable, writer);
        if (!writer->m_source)
            delete writer;
    }

private:
    PipeWriter(int fd, std::shared_ptr<const std::string> data)
        : m_fd(fd)
        , m_data(std::move(data))
    {
    }

    ~PipeWriter()
    {
        if (m_source)
            wl_event_source_remove(m_source);
        close(m_fd);
    }

    static int on_writable(int fd, uint32_t mask, void* data)
    {
        auto* self = static_cast<PipeWriter*>(data);
        if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
            delete self;
            return 0;
        }
        const std::string& buffer = *self->m_data;
        ssize_t n = write(fd, buffer.data() + self->m_offset, buffer.size() - self->m_offset);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return 0;
        if (n > 0)
            self->m_offset += static_cast<std::size_t>(n);
        if (n <= 0 || self->m_offset == buffer.size())
            delete self;
        return 0;
    }

    int m_fd;
    std::shared_ptr<const std::string> m_data;
    std::size_t m_offset = 0;
    wl_event_source* m_source = nullptr;
};

} // namespace

// Reads a Wayland data source's offer through a pipe, asynchronously.
class PipeReader {
public:
    using Done = std::function<void(std::optional<std::string>)>;

    PipeReader(wl_event_loop* loop, int fd, Done done)
        : m_fd(fd)
        , m_done(std::move(done))
    {
        m_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, &PipeReader::on_readable, this);
        m_timer = wl_event_loop_add_timer(loop, &PipeReader::on_timeout, this);
        if (m_timer)
            wl_event_source_timer_update(m_timer, transfer_timeout_ms);
    }

    ~PipeReader()
    {
        if (m_source)
            wl_event_source_remove(m_source);
        if (m_timer)
            wl_event_source_remove(m_timer);
        close(m_fd);
    }

    PipeReader(const PipeReader&) = delete;
    PipeReader& operator=(const PipeReader&) = delete;

private:
    static int on_readable(int fd, uint32_t mask, void* data)
    {
        auto* self = static_cast<PipeReader*>(data);
        char buffer[16 * 1024];
        for (;;) {
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                if (self->m_data.size() + static_cast<std::size_t>(n) > max_clipboard_bytes) {
                    wlr_log(WLR_INFO, "Clipboard: selection larger than %zu bytes, dropping", max_clipboard_bytes);
                    self->finish(std::nullopt);
                    return 0;
                }
                self->m_data.append(buffer, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && errno == EAGAIN)
                return 0;
            // EOF, or an error: either way the transfer is over.
            if (n == 0)
                self->finish(std::move(self->m_data));
            else
                self->finish(std::nullopt);
            return 0;
        }
    }

    static int on_timeout(void* data)
    {
        wlr_log(WLR_INFO, "Clipboard: timed out reading the selection");
        static_cast<PipeReader*>(data)->finish(std::nullopt);
        return 0;
    }

    // May destroy `this` via the callback; must be the last thing called.
    void finish(std::optional<std::string> result)
    {
        if (auto done = std::exchange(m_done, nullptr))
            done(std::move(result));
    }

    int m_fd;
    Done m_done;
    std::string m_data;
    wl_event_source* m_source = nullptr;
    wl_event_source* m_timer = nullptr;
};

// --- Remote (RDP client) data source ------------------------------------------

struct Clipboard::RemoteSource {
    wlr_data_source base; // must stay first
    Clipboard* owner;

    static RemoteSource* from(wlr_data_source* source) { return reinterpret_cast<RemoteSource*>(source); }

    static void send(wlr_data_source* source, const char* mime_type, int32_t fd)
    {
        if (Clipboard* owner = from(source)->owner)
            owner->paste_remote(fd);
        else
            close(fd);
    }

    static void destroy(wlr_data_source* source)
    {
        auto* self = from(source);
        if (self->owner && self->owner->m_remote_source == self)
            self->owner->m_remote_source = nullptr;
        delete self;
    }

    static constexpr wlr_data_source_impl impl = {
        .send = &RemoteSource::send,
        .accept = nullptr,
        .destroy = &RemoteSource::destroy,
        .dnd_drop = nullptr,
        .dnd_finish = nullptr,
        .dnd_action = nullptr,
    };

    static bool is(const wlr_data_source* source) { return source && source->impl == &impl; }

    static RemoteSource* create(Clipboard* owner)
    {
        auto* self = new RemoteSource{};
        self->owner = owner;
        wlr_data_source_init(&self->base, &impl);
        for (const char* mime : text_mime_types) {
            auto** slot = static_cast<char**>(wl_array_add(&self->base.mime_types, sizeof(char*)));
            if (slot)
                *slot = strdup(mime); // freed by wlr_data_source_destroy
        }
        return self;
    }
};

// --- FreeRDP callbacks ----------------------------------------------------------

struct ClipboardCallbacks {
    static Clipboard* from(CliprdrServerContext* context) { return static_cast<Clipboard*>(context->custom); }

    static UINT client_capabilities(CliprdrServerContext* context, const CLIPRDR_CAPABILITIES* capabilities)
    {
        Clipboard* self = from(context);
        self->m_ready = true;
        if (!self->m_locked)
            self->announce_local();
        return CHANNEL_RC_OK;
    }

    static UINT client_format_list(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST* list)
    {
        Clipboard* self = from(context);

        CLIPRDR_FORMAT_LIST_RESPONSE response{};
        response.common.msgType = CB_FORMAT_LIST_RESPONSE;
        response.common.msgFlags = CB_RESPONSE_OK;
        (void)context->ServerFormatListResponse(context, &response);

        self->m_ready = true;
        self->m_remote_has_text = false;
        for (UINT32 i = 0; i < list->numFormats; ++i) {
            if (list->formats[i].formatId == CF_UNICODETEXT)
                self->m_remote_has_text = true;
        }

        // New client clipboard: anything cached or in flight is stale.
        self->m_remote_cache.reset();
        self->fail_waiting_pastes();
        if (std::exchange(self->m_remote_request_pending, false)) {
            ++self->m_stale_responses;
            wl_event_source_timer_update(self->m_remote_timer, 0);
        }

        if (self->m_locked) {
            self->m_remote_apply_on_unlock = true;
            return CHANNEL_RC_OK;
        }
        self->apply_remote_selection();
        return CHANNEL_RC_OK;
    }

    static UINT client_format_list_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* response)
    {
        if (response->common.msgFlags & CB_RESPONSE_FAIL)
            wlr_log(WLR_INFO, "Clipboard: client rejected our format list");
        return CHANNEL_RC_OK;
    }

    static UINT client_format_data_request(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_REQUEST* request)
    {
        Clipboard* self = from(context);
        if (self->m_locked || request->requestedFormatId != CF_UNICODETEXT) {
            self->send_data_response(nullptr);
            return CHANNEL_RC_OK;
        }
        self->m_client_request_pending = true;
        self->answer_client_request();
        return CHANNEL_RC_OK;
    }

    static UINT client_format_data_response(CliprdrServerContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* response)
    {
        Clipboard* self = from(context);
        if (self->m_stale_responses > 0) {
            --self->m_stale_responses;
            return CHANNEL_RC_OK;
        }
        if (!self->m_remote_request_pending)
            return CHANNEL_RC_OK;

        self->m_remote_request_pending = false;
        wl_event_source_timer_update(self->m_remote_timer, 0);

        const UINT32 length = response->common.dataLen;
        if (!(response->common.msgFlags & CB_RESPONSE_OK) || length < sizeof(WCHAR) || !response->requestedFormatData
            || length > max_clipboard_bytes) {
            self->fail_waiting_pastes();
            return CHANNEL_RC_OK;
        }

        size_t utf8_length = 0;
        char* utf8 = ConvertWCharNToUtf8Alloc(reinterpret_cast<const WCHAR*>(response->requestedFormatData),
            length / sizeof(WCHAR), &utf8_length);
        if (!utf8) {
            self->fail_waiting_pastes();
            return CHANNEL_RC_OK;
        }
        self->m_remote_cache = std::make_shared<const std::string>(from_crlf(std::string_view(utf8, utf8_length)));
        std::free(utf8);

        for (int fd : std::exchange(self->m_waiting_pastes, {}))
            PipeWriter::start(self->m_loop, fd, self->m_remote_cache);
        return CHANNEL_RC_OK;
    }
};

// --- Clipboard ------------------------------------------------------------------

std::unique_ptr<Clipboard> Clipboard::create(Server& server, void* vcm)
{
    auto clipboard = std::make_unique<Clipboard>(server);
    if (!clipboard->open(vcm))
        return nullptr;
    return clipboard;
}

Clipboard::Clipboard(Server& server)
    : m_server(server)
    , m_seat(server.seat())
    , m_loop(server.event_loop())
    , m_locked(server.locked())
{
}

bool Clipboard::open(void* vcm)
{
    m_context = cliprdr_server_context_new(vcm);
    if (!m_context)
        return false;

    m_context->custom = this;
    m_context->useLongFormatNames = TRUE;
    m_context->streamFileClipEnabled = FALSE;
    m_context->fileClipNoFilePaths = TRUE;
    m_context->canLockClipData = FALSE;
    // We drive the channel from the Wayland event loop (Open, not Start), so
    // the initialisation sequence is sent by hand below.
    m_context->autoInitializationSequence = FALSE;

    m_context->ClientCapabilities = &ClipboardCallbacks::client_capabilities;
    m_context->ClientFormatList = &ClipboardCallbacks::client_format_list;
    m_context->ClientFormatListResponse = &ClipboardCallbacks::client_format_list_response;
    m_context->ClientFormatDataRequest = &ClipboardCallbacks::client_format_data_request;
    m_context->ClientFormatDataResponse = &ClipboardCallbacks::client_format_data_response;

    if (m_context->Open(m_context) != CHANNEL_RC_OK) {
        wlr_log(WLR_ERROR, "Clipboard: cannot open the cliprdr channel");
        return false;
    }

    int fd = GetEventFileDescriptor(m_context->GetEventHandle(m_context));
    if (fd < 0)
        return false;
    m_channel_source = wl_event_loop_add_fd(m_loop, fd, WL_EVENT_READABLE, &Clipboard::on_channel_event, this);
    m_remote_timer = wl_event_loop_add_timer(m_loop, &Clipboard::on_remote_timeout, this);
    if (!m_channel_source || !m_remote_timer)
        return false;

    CLIPRDR_GENERAL_CAPABILITY_SET general{};
    general.capabilitySetType = CB_CAPSTYPE_GENERAL;
    general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    general.version = CB_CAPS_VERSION_2;
    general.generalFlags = CB_USE_LONG_FORMAT_NAMES;

    CLIPRDR_CAPABILITIES capabilities{};
    capabilities.common.msgType = CB_CLIP_CAPS;
    capabilities.cCapabilitiesSets = 1;
    capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);

    CLIPRDR_MONITOR_READY ready{};
    ready.common.msgType = CB_MONITOR_READY;

    if (m_context->ServerCapabilities(m_context, &capabilities) != CHANNEL_RC_OK
        || m_context->MonitorReady(m_context, &ready) != CHANNEL_RC_OK) {
        wlr_log(WLR_ERROR, "Clipboard: cliprdr initialisation failed");
        return false;
    }

    m_set_selection.connect(&m_seat->events.set_selection, [this](void*) { on_selection_changed(); });
    wlr_log(WLR_INFO, "Clipboard: cliprdr channel open");
    return true;
}

Clipboard::~Clipboard()
{
    m_set_selection.disconnect();
    m_local_reader.reset();
    fail_waiting_pastes();

    // The client's data is unreachable once the channel is gone.
    if (m_remote_source) {
        m_remote_source->owner = nullptr;
        if (m_seat->selection_source == &m_remote_source->base)
            wlr_seat_set_selection(m_seat, nullptr, wl_display_next_serial(m_seat->display));
        m_remote_source = nullptr;
    }

    if (m_remote_timer)
        wl_event_source_remove(m_remote_timer);
    if (m_channel_source)
        wl_event_source_remove(m_channel_source);
    if (m_context) {
        m_context->Close(m_context);
        cliprdr_server_context_free(m_context);
    }
}

void Clipboard::set_locked(bool locked)
{
    if (locked == m_locked)
        return;
    m_locked = locked;

    if (locked) {
        // Pastes in progress must not complete into a locked desktop.
        fail_waiting_pastes();
        return;
    }

    // On unlock, the newest copy wins: a copy made on the client while we
    // were locked, otherwise whatever the desktop holds.
    if (std::exchange(m_remote_apply_on_unlock, false) && m_remote_has_text)
        apply_remote_selection();
    else
        announce_local();
}

int Clipboard::on_channel_event(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<Clipboard*>(data);
    if (self->m_context->CheckEventHandle(self->m_context) != CHANNEL_RC_OK)
        wlr_log(WLR_DEBUG, "Clipboard: cliprdr channel error");
    return 0;
}

// --- RDP → Wayland ------------------------------------------------------------------

void Clipboard::apply_remote_selection()
{
    m_setting_selection = true;
    if (m_remote_has_text) {
        // Replacing the selection destroys any previous RemoteSource.
        RemoteSource* source = RemoteSource::create(this);
        wlr_seat_set_selection(m_seat, &source->base, wl_display_next_serial(m_seat->display));
        m_remote_source = source;
        // The desktop's own text is no longer what the client sees.
        m_local_reader.reset();
        m_local_cache.reset();
    } else if (m_remote_source && m_seat->selection_source == &m_remote_source->base) {
        // The client copied something we cannot represent.
        wlr_seat_set_selection(m_seat, nullptr, wl_display_next_serial(m_seat->display));
    }
    m_setting_selection = false;
}

void Clipboard::paste_remote(int fd)
{
    if (m_locked) {
        close(fd);
        return;
    }
    if (m_remote_cache) {
        PipeWriter::start(m_loop, fd, m_remote_cache);
        return;
    }
    m_waiting_pastes.push_back(fd);
    if (!m_remote_request_pending)
        request_remote();
}

void Clipboard::request_remote()
{
    CLIPRDR_FORMAT_DATA_REQUEST request{};
    request.common.msgType = CB_FORMAT_DATA_REQUEST;
    request.requestedFormatId = CF_UNICODETEXT;
    if (m_context->ServerFormatDataRequest(m_context, &request) != CHANNEL_RC_OK) {
        fail_waiting_pastes();
        return;
    }
    m_remote_request_pending = true;
    wl_event_source_timer_update(m_remote_timer, transfer_timeout_ms);
}

int Clipboard::on_remote_timeout(void* data)
{
    auto* self = static_cast<Clipboard*>(data);
    if (self->m_remote_request_pending) {
        wlr_log(WLR_INFO, "Clipboard: client did not answer a paste request");
        self->m_remote_request_pending = false;
        self->fail_waiting_pastes();
    }
    return 0;
}

void Clipboard::fail_waiting_pastes()
{
    for (int fd : std::exchange(m_waiting_pastes, {}))
        close(fd);
}

// --- Wayland → RDP ------------------------------------------------------------------

void Clipboard::on_selection_changed()
{
    if (m_setting_selection || RemoteSource::is(m_seat->selection_source))
        return;

    // A cleared selection (usually the copying app exiting) keeps what the
    // client already has: the text is cached, so pasting there still works.
    if (!m_seat->selection_source)
        return;

    m_local_reader.reset();
    m_local_cache.reset();
    if (!m_locked)
        announce_local();
}

void Clipboard::announce_local()
{
    if (!m_ready || m_locked)
        return;

    wlr_data_source* source = m_seat->selection_source;
    if (RemoteSource::is(source))
        return; // the client already owns this content

    const bool has_text = m_local_cache || (source && preferred_text_mime(source));
    send_format_list(has_text);
    if (has_text && !m_local_cache && !m_local_reader)
        fetch_local(source);
}

void Clipboard::fetch_local(wlr_data_source* source)
{
    const char* mime = preferred_text_mime(source);
    int fds[2];
    if (!mime || pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return;

    // The source takes ownership of the write end.
    wlr_data_source_send(source, mime, fds[1]);
    m_local_reader = std::make_unique<PipeReader>(m_loop, fds[0], [this](std::optional<std::string> text) {
        m_local_cache = std::move(text);
        // Destroys the reader that is calling us; nothing below touches it.
        m_local_reader.reset();
        if (m_local_cache) {
            answer_client_request();
        } else if (std::exchange(m_client_request_pending, false)) {
            // Do not retry a source that just failed.
            send_data_response(nullptr);
        }
    });
}

void Clipboard::answer_client_request()
{
    if (!m_client_request_pending)
        return;
    if (m_locked) {
        m_client_request_pending = false;
        send_data_response(nullptr);
        return;
    }
    if (m_local_cache) {
        m_client_request_pending = false;
        send_data_response(&*m_local_cache);
        return;
    }
    if (m_local_reader)
        return; // answered when the read completes

    wlr_data_source* source = m_seat->selection_source;
    if (source && !RemoteSource::is(source) && preferred_text_mime(source)) {
        fetch_local(source);
        if (m_local_reader)
            return;
    }
    m_client_request_pending = false;
    send_data_response(nullptr);
}

void Clipboard::send_format_list(bool has_text)
{
    CLIPRDR_FORMAT text_format{};
    text_format.formatId = CF_UNICODETEXT;

    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = has_text ? 1 : 0;
    list.formats = has_text ? &text_format : nullptr;
    (void)m_context->ServerFormatList(m_context, &list);
}

void Clipboard::send_data_response(const std::string* utf8)
{
    CLIPRDR_FORMAT_DATA_RESPONSE response{};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_FAIL;

    WCHAR* wide = nullptr;
    if (utf8) {
        const std::string crlf = to_crlf(*utf8);
        size_t wide_length = 0;
        wide = ConvertUtf8NToWCharAlloc(crlf.data(), crlf.size(), &wide_length);
        if (wide) {
            response.common.msgFlags = CB_RESPONSE_OK;
            // Include the terminating NUL, as CF_UNICODETEXT requires.
            response.common.dataLen = static_cast<UINT32>((wide_length + 1) * sizeof(WCHAR));
            response.requestedFormatData = reinterpret_cast<const BYTE*>(wide);
        }
    }

    (void)m_context->ServerFormatDataResponse(m_context, &response);
    std::free(wide);
}

} // namespace wfa
