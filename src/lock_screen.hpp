#pragma once

#include <functional>
#include <string>

#include "authenticator.hpp"
#include "wlr.hpp"

namespace wfa {

// Compositor-internal lock screen. It is drawn by the compositor itself
// (not by a client), so nothing can race it at startup or after an RDP
// disconnect: while locked, the desktop scene tree is disabled and no
// client receives input.
class LockScreen {
public:
    LockScreen(wl_event_loop* loop, wlr_scene_tree* parent, Authenticator& authenticator);
    ~LockScreen();

    LockScreen(const LockScreen&) = delete;
    LockScreen& operator=(const LockScreen&) = delete;

    std::function<void()> on_unlock;

    bool locked() const { return m_locked; }
    void lock();

    // The background covers the whole desktop; the clock and password field
    // are centred on the primary monitor, so they never straddle a bezel.
    void set_layout(int width, int height, wlr_box primary);

    // Feeds one key press while locked. `utf8` is the text the key produces
    // under the current xkb state (empty for non-printing keys).
    void handle_key(xkb_keysym_t sym, std::string_view utf8);

    // Shows a "Caps Lock is on" hint, since an unexpected Caps Lock is the
    // usual reason a correct password is rejected.
    void set_caps_lock(bool on);

private:
    static int on_tick(void* data);
    void submit();
    void clear_password();
    void render();

    wl_event_loop* m_loop;
    Authenticator& m_authenticator;

    wlr_scene_tree* m_tree;
    wlr_scene_buffer* m_buffer_node;
    wl_event_source* m_timer;

    int m_width = 0;
    int m_height = 0;
    wlr_box m_primary{};
    bool m_locked = true;
    // Bumped on every lock() so an authentication started before a re-lock
    // (e.g. an RDP takeover) can never unlock the new session.
    unsigned m_generation = 0;

    std::string m_password;
    std::string m_status;
    bool m_status_is_error = false;
    std::string m_rendered_clock;
    bool m_caps_lock = false;
};

} // namespace wfa
