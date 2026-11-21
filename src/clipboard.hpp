#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "listener.hpp"
#include "wlr.hpp"

typedef struct s_cliprdr_server_context CliprdrServerContext;

namespace wfa {

class Server;
class PipeReader;

// Bridges the Wayland clipboard (the seat selection) and the RDP cliprdr
// channel of one peer. Plain text only: CF_UNICODETEXT on the RDP side,
// text/plain;charset=utf-8 on the Wayland side.
//
// Nothing crosses in either direction while the session is locked: a peer
// sitting at the lock screen can neither read the desktop's clipboard nor
// inject into it. A copy made on the client while locked is applied on unlock.
class Clipboard {
public:
    // `vcm` is the peer's WTS virtual channel manager. Returns nullptr if the
    // channel cannot be opened.
    static std::unique_ptr<Clipboard> create(Server& server, void* vcm);
    ~Clipboard();

    Clipboard(const Clipboard&) = delete;
    Clipboard& operator=(const Clipboard&) = delete;

    void set_locked(bool locked);

    // Public for std::make_unique; use create().
    explicit Clipboard(Server& server);

private:
    struct RemoteSource;

    bool open(void* vcm);
    static int on_channel_event(int fd, uint32_t mask, void* data);
    static int on_remote_timeout(void* data);

    // RDP → Wayland
    void apply_remote_selection();
    void paste_remote(int fd);
    void request_remote();
    void fail_waiting_pastes();

    // Wayland → RDP
    void on_selection_changed();
    void announce_local();
    void fetch_local(wlr_data_source* source);
    void answer_client_request();
    void send_format_list(bool has_text);
    void send_data_response(const std::string* utf8);

    // FreeRDP channel callbacks, defined in clipboard.cpp.
    friend struct ClipboardCallbacks;

    Server& m_server;
    wlr_seat* m_seat;
    wl_event_loop* m_loop;

    CliprdrServerContext* m_context = nullptr;
    wl_event_source* m_channel_source = nullptr;
    bool m_ready = false;
    bool m_locked = true;

    // Set while we change the seat selection ourselves, so the resulting
    // set_selection event is not echoed back to the client.
    bool m_setting_selection = false;
    Listener m_set_selection;

    // RDP → Wayland state.
    bool m_remote_has_text = false;
    bool m_remote_apply_on_unlock = false;
    RemoteSource* m_remote_source = nullptr;
    std::shared_ptr<const std::string> m_remote_cache;
    bool m_remote_request_pending = false;
    // Responses still due for requests superseded by a newer client copy.
    // cliprdr is ordered, so these arrive before any newer response.
    unsigned m_stale_responses = 0;
    std::vector<int> m_waiting_pastes;
    wl_event_source* m_remote_timer = nullptr;

    // Wayland → RDP state.
    std::unique_ptr<PipeReader> m_local_reader;
    std::optional<std::string> m_local_cache;
    bool m_client_request_pending = false;
};

} // namespace wfa
