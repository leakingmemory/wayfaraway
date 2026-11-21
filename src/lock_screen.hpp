#pragma once

#include <cstdint>
#include <functional>
#include <optional>
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

    // Feeds key presses and releases while locked. `utf8` is the text the
    // key produces under the current xkb state (empty for non-printing
    // keys). `keycode` identifies the key so a held key auto-repeats until
    // it is released; RDP clients' own repeats are dropped before this.
    void key_pressed(std::uint32_t keycode, xkb_keysym_t sym, std::string_view utf8);
    void key_released(std::uint32_t keycode);

    // Shows a "Caps Lock is on" hint, since an unexpected Caps Lock is the
    // usual reason a correct password is rejected.
    void set_caps_lock(bool on);

private:
    static int on_tick(void* data);
    static int on_repeat(void* data);
    static int on_blink(void* data);
    void handle_key(xkb_keysym_t sym, std::string_view utf8);
    void stop_repeat();
    void restart_blink();
    void update_caret();
    void submit();
    void clear_password();
    void render();

    wl_event_loop* m_loop;
    Authenticator& m_authenticator;

    wlr_scene_tree* m_tree;
    wlr_scene_buffer* m_buffer_node;
    // A separate node, so blinking repaints a few pixels instead of the
    // whole desktop-sized buffer (and over RDP, sends that little).
    wlr_scene_rect* m_caret;
    wl_event_source* m_timer;
    wl_event_source* m_blink_timer;
    wl_event_source* m_repeat_timer;

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

    wlr_box m_caret_box{};
    bool m_caret_on = true;

    std::optional<std::uint32_t> m_repeat_keycode;
    xkb_keysym_t m_repeat_sym = XKB_KEY_NoSymbol;
    std::string m_repeat_utf8;
};

} // namespace wfa
