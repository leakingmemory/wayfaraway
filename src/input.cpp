#include <algorithm>
#include <chrono>
#include <span>
#include <utility>

#include <linux/input-event-codes.h>

#include "program_manager.hpp"
#include "desktop_icons.hpp"
#include "server.hpp"
#include "view.hpp"

namespace wfa {

namespace {

std::uint32_t now_msec()
{
    using namespace std::chrono;
    return static_cast<std::uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

// --- Keyboard ----------------------------------------------------------------

void Server::setup_keyboard()
{
    // The headless backend has no input devices; RDP input is fed into this
    // virtual keyboard so xkb state, modifiers and the seat behave normally.
    static constexpr wlr_keyboard_impl keyboard_impl = {
        .name = "rdp-keyboard",
        .led_update = &Server::on_led_update,
    };
    wlr_keyboard_init(&m_keyboard, &keyboard_impl, "rdp-keyboard");
    // Set before the keymap, which already reports LEDs.
    m_keyboard.data = this;

    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    // nullptr names → honours XKB_DEFAULT_LAYOUT etc. from the environment.
    xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(&m_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    wlr_keyboard_set_repeat_info(&m_keyboard, 25, 600);
    wlr_seat_set_keyboard(m_seat, &m_keyboard);

    m_keyboard_key.connect(&m_keyboard.events.key, [this](void* data) {
        on_key(*static_cast<wlr_keyboard_key_event*>(data));
    });
    m_keyboard_modifiers.connect(&m_keyboard.events.modifiers, [this](void*) { on_modifiers(); });
}

void Server::on_modifiers()
{
    // Covers both Caps Lock presses and synchronize events from the client.
    const bool caps_lock
        = xkb_state_mod_name_is_active(m_keyboard.xkb_state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) > 0;
    if (m_lock_screen)
        m_lock_screen->set_caps_lock(caps_lock);
    if (!locked())
        wlr_seat_keyboard_notify_modifiers(m_seat, &m_keyboard.modifiers);
}

void Server::on_led_update(wlr_keyboard* keyboard, std::uint32_t leds)
{
    auto* self = static_cast<Server*>(keyboard->data);
    if (!self || !self->on_keyboard_leds)
        return;

    // WLR_LED_* to RDP's KBD_SYNC_* bits (scroll 1, num 2, caps 4, kana 8).
    std::uint32_t flags = 0;
    if (leds & WLR_LED_SCROLL_LOCK)
        flags |= 0x1;
    if (leds & WLR_LED_NUM_LOCK)
        flags |= 0x2;
    if (leds & WLR_LED_CAPS_LOCK)
        flags |= 0x4;
    if (leds & WLR_LED_KANA)
        flags |= 0x8;
    self->on_keyboard_leds(flags);
}

void Server::sync_lock_keys(bool caps_lock, bool num_lock)
{
    xkb_keymap* keymap = m_keyboard.keymap;
    xkb_state* state = m_keyboard.xkb_state;
    if (!keymap || !state)
        return;

    // Resolved through the keymap, so this works whichever real modifier a
    // layout binds NumLock to. Scroll Lock has no modifier in standard
    // keymaps (it only drives an LED), so it is not synced.
    const xkb_mod_mask_t caps_mask = xkb_keymap_mod_get_mask(keymap, XKB_MOD_NAME_CAPS);
    const xkb_mod_mask_t num_mask = xkb_keymap_mod_get_mask(keymap, XKB_VMOD_NAME_NUM);

    const xkb_mod_mask_t locked = xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED);
    xkb_mod_mask_t wanted = locked & ~(caps_mask | num_mask);
    if (caps_lock)
        wanted |= caps_mask;
    if (num_lock)
        wanted |= num_mask;
    if (wanted == locked)
        return;

    wlr_keyboard_notify_modifiers(&m_keyboard, xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED), wanted,
        xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LOCKED));
}

void Server::inject_key(std::uint32_t evdev_keycode, bool pressed)
{
    std::span held(m_keyboard.keycodes, m_keyboard.num_keycodes);
    const bool is_held = std::ranges::contains(held, evdev_keycode);
    // RDP clients send auto-repeat as repeated presses; Wayland clients do
    // their own repeat, so drop them (and stray releases).
    if (pressed == is_held)
        return;

    wlr_keyboard_key_event event{
        .time_msec = now_msec(),
        .keycode = evdev_keycode,
        .update_state = true,
        .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
    };
    wlr_keyboard_notify_key(&m_keyboard, &event);
}

void Server::on_key(const wlr_keyboard_key_event& event)
{
    const std::uint32_t xkb_keycode = event.keycode + 8;
    const bool pressed = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;

    if (locked()) {
        if (pressed) {
            xkb_keysym_t sym = xkb_state_key_get_one_sym(m_keyboard.xkb_state, xkb_keycode);
            char utf8[64]{};
            int len = xkb_state_key_get_utf8(m_keyboard.xkb_state, xkb_keycode, utf8, sizeof(utf8));
            m_lock_screen->key_pressed(event.keycode, sym,
                std::string_view(utf8, std::clamp(len, 0, static_cast<int>(sizeof(utf8) - 1))));
            explicit_bzero(utf8, sizeof(utf8));
        } else {
            m_lock_screen->key_released(event.keycode);
        }
        return;
    }

    bool handled = false;
    if (pressed) {
        const std::uint32_t modifiers = wlr_keyboard_get_modifiers(&m_keyboard);
        const xkb_keysym_t* syms = nullptr;
        int count = xkb_state_key_get_syms(m_keyboard.xkb_state, xkb_keycode, &syms);
        for (int i = 0; i < count && !handled; ++i)
            handled = handle_keybinding(syms[i], modifiers);
    }

    if (handled)
        return;

    // The launcher is not a client: it gets keys straight from here.
    if (m_programs_focused) {
        if (pressed)
            m_programs->key(xkb_state_key_get_one_sym(m_keyboard.xkb_state, xkb_keycode));
        return;
    }
    wlr_seat_keyboard_notify_key(m_seat, event.time_msec, event.keycode, event.state);
}

bool Server::handle_keybinding(xkb_keysym_t sym, std::uint32_t modifiers)
{
    const bool alt = modifiers & WLR_MODIFIER_ALT;
    const bool ctrl = modifiers & WLR_MODIFIER_CTRL;

    if (ctrl && alt && (sym == XKB_KEY_l || sym == XKB_KEY_L)) {
        lock();
        return true;
    }
    if (ctrl && !alt && sym == XKB_KEY_Escape) {
        show_programs();
        return true;
    }
    if (!alt || ctrl)
        return false;

    switch (sym) {
    case XKB_KEY_Return:
        spawn(m_config.terminal);
        return true;
    case XKB_KEY_Tab:
        cycle_focus();
        return true;
    case XKB_KEY_F4:
        if (m_programs_focused)
            minimize_programs();
        else if (!m_views.empty() && m_views.front()->mapped())
            m_views.front()->close();
        return true;
    default:
        return false;
    }
}

// --- Pointer -----------------------------------------------------------------

void Server::inject_pointer_motion(double x, double y)
{
    wlr_cursor_warp_closest(m_cursor, nullptr, x, y);
    if (m_programs_grab) {
        m_programs->motion(m_cursor->x, m_cursor->y);
        return;
    }
    process_cursor_motion(now_msec());
}

void Server::inject_pointer_button(std::uint32_t button, bool pressed)
{
    if (pressed ? !m_pressed_buttons.insert(button).second : m_pressed_buttons.erase(button) == 0)
        return;

    if (locked())
        return;

    const std::uint32_t time = now_msec();

    if (!pressed) {
        if (m_programs_grab) {
            m_programs_grab = false;
            m_programs->release(m_cursor->x, m_cursor->y);
            return;
        }
        if (m_cursor_mode != CursorMode::Passthrough) {
            reset_cursor_mode();
            process_cursor_motion(time);
            return;
        }
        if (m_frame_view) {
            frame_release(m_cursor->x, m_cursor->y);
            return;
        }
        wlr_seat_pointer_notify_button(m_seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
        wlr_seat_pointer_notify_frame(m_seat);
        return;
    }

    const Hit hit = hit_test(m_cursor->x, m_cursor->y);
    switch (hit.kind) {
    case Hit::Kind::Programs:
        if (button == BTN_LEFT)
            m_programs_grab = m_programs->press(m_cursor->x, m_cursor->y, time);
        else
            focus_programs();
        return;
    case Hit::Kind::Icons:
        if (button == BTN_LEFT)
            m_desktop_icons->press(m_cursor->x, m_cursor->y, time);
        return;
    case Hit::Kind::None:
        return; // the bare desktop
    case Hit::Kind::View:
        break;
    }

    View* view = hit.view;
    if (!hit.surface) {
        if (button == BTN_LEFT)
            frame_press(view, view->frame_hit(m_cursor->x, m_cursor->y), time);
        else
            focus_view(view);
        return;
    }

    {
        focus_view(view);
        // Alt+drag moves, Alt+right-drag resizes from the nearest corner.
        if (wlr_keyboard_get_modifiers(&m_keyboard) & WLR_MODIFIER_ALT) {
            if (button == BTN_LEFT) {
                begin_interactive(view, false, 0);
                return;
            }
            if (button == BTN_RIGHT) {
                const wlr_box geo = view->toplevel()->base->geometry;
                std::uint32_t edges = 0;
                edges |= (m_cursor->x - view->x() < geo.width / 2.0) ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
                edges |= (m_cursor->y - view->y() < geo.height / 2.0) ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
                begin_interactive(view, true, edges);
                return;
            }
        }
    }

    wlr_seat_pointer_notify_button(m_seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_pointer_notify_frame(m_seat);
}

void Server::inject_pointer_axis(wl_pointer_axis axis, double value, int value120)
{
    if (locked())
        return;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && hit_test(m_cursor->x, m_cursor->y).kind == Hit::Kind::Programs) {
        m_programs->scroll(m_cursor->x, m_cursor->y, value);
        return;
    }
    wlr_seat_pointer_notify_axis(m_seat, now_msec(), axis, value, value120, WL_POINTER_AXIS_SOURCE_WHEEL,
        WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    wlr_seat_pointer_notify_frame(m_seat);
}

void Server::release_all_input()
{
    std::vector<std::uint32_t> keys(m_keyboard.keycodes, m_keyboard.keycodes + m_keyboard.num_keycodes);
    for (std::uint32_t key : keys)
        inject_key(key, false);

    std::vector<std::uint32_t> buttons(m_pressed_buttons.begin(), m_pressed_buttons.end());
    for (std::uint32_t button : buttons)
        inject_pointer_button(button, false);

    reset_cursor_mode();
}

void Server::process_cursor_motion(std::uint32_t time_msec)
{
    if (m_cursor_mode == CursorMode::Move) {
        process_cursor_move();
        return;
    }
    if (m_cursor_mode == CursorMode::Resize) {
        process_cursor_resize();
        return;
    }

    if (locked()) {
        wlr_seat_pointer_notify_clear_focus(m_seat);
        return;
    }

    const Hit hit = hit_test(m_cursor->x, m_cursor->y);
    update_cursor_image(hit);

    if (hit.surface) {
        wlr_seat_pointer_notify_enter(m_seat, hit.surface, hit.sx, hit.sy);
        wlr_seat_pointer_notify_motion(m_seat, time_msec, hit.sx, hit.sy);
    } else {
        wlr_seat_pointer_clear_focus(m_seat);
    }
    wlr_seat_pointer_notify_frame(m_seat);
}

void Server::update_cursor_image(const Hit& hit)
{
    if (hit.surface) {
        // The client sets its own; ours must be set again when it leaves.
        m_cursor_image = nullptr;
        return;
    }

    const char* image = "default";
    if (hit.kind == Hit::Kind::View) {
        const retro::FrameHit frame = hit.view->frame_hit(m_cursor->x, m_cursor->y);
        if (frame.part == retro::FramePart::Edge) {
            // CSS cursor names, with the legacy X11 name for older themes.
            struct Names {
                const char* css;
                const char* x11;
            };
            const bool top = frame.edges & WLR_EDGE_TOP, bottom = frame.edges & WLR_EDGE_BOTTOM;
            const bool left = frame.edges & WLR_EDGE_LEFT, right = frame.edges & WLR_EDGE_RIGHT;
            const Names names = top ? (left ? Names{ "nw-resize", "top_left_corner" }
                                          : right ? Names{ "ne-resize", "top_right_corner" }
                                                  : Names{ "n-resize", "top_side" })
                : bottom ? (left ? Names{ "sw-resize", "bottom_left_corner" }
                                 : right ? Names{ "se-resize", "bottom_right_corner" }
                                         : Names{ "s-resize", "bottom_side" })
                : left ? Names{ "w-resize", "left_side" }
                       : Names{ "e-resize", "right_side" };
            image = wlr_xcursor_manager_get_xcursor(m_cursor_manager, names.css, 1) ? names.css : names.x11;
        }
    }
    if (image != m_cursor_image) {
        m_cursor_image = image;
        wlr_cursor_set_xcursor(m_cursor, m_cursor_manager, image);
    }
}

void Server::frame_press(View* view, retro::FrameHit hit, std::uint32_t time_msec)
{
    focus_view(view);
    switch (hit.part) {
    case retro::FramePart::Title:
        if (m_frame_clicks.click(view, static_cast<int>(hit.part), time_msec, m_cursor->x, m_cursor->y))
            view->toggle_maximized();
        else if (!view->maximized())
            begin_interactive(view, false, 0);
        break;
    case retro::FramePart::Edge:
        begin_interactive(view, true, hit.edges);
        break;
    case retro::FramePart::Control:
        // Double-clicking the control-menu box closes the window.
        if (m_frame_clicks.click(view, static_cast<int>(hit.part), time_msec, m_cursor->x, m_cursor->y))
            view->close();
        break;
    case retro::FramePart::Minimize:
    case retro::FramePart::Maximize:
        // Buttons act on release, and only if still over them.
        m_frame_view = view;
        m_frame_part = hit.part;
        break;
    default:
        break;
    }
}

void Server::frame_release(double lx, double ly)
{
    View* view = std::exchange(m_frame_view, nullptr);
    const retro::FramePart part = std::exchange(m_frame_part, retro::FramePart::None);
    if (!view || view->frame_hit(lx, ly).part != part)
        return;
    if (part == retro::FramePart::Minimize)
        minimize_view(view);
    else if (part == retro::FramePart::Maximize)
        view->toggle_maximized();
}

Server::Hit Server::hit_test(double lx, double ly) const
{
    Hit hit;
    double sx = 0, sy = 0;
    wlr_scene_node* node = wlr_scene_node_at(&m_desktop_tree->node, lx, ly, &sx, &sy);
    if (!node)
        return hit;

    wlr_surface* surface = nullptr;
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        if (auto* scene_surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node)))
            surface = scene_surface->surface;
    }

    for (wlr_scene_tree* tree = node->parent; tree; tree = tree->node.parent) {
        if (tree == m_programs->tree()) {
            hit.kind = Hit::Kind::Programs;
            return hit;
        }
        if (tree == m_desktop_icons->tree()) {
            hit.kind = Hit::Kind::Icons;
            return hit;
        }
        // Only View roots carry data. A buffer that is not a surface is the
        // window frame.
        if (tree->node.data) {
            hit.kind = Hit::Kind::View;
            hit.view = static_cast<View*>(tree->node.data);
            hit.surface = surface;
            hit.sx = sx;
            hit.sy = sy;
            return hit;
        }
    }
    return hit;
}

void Server::begin_interactive(View* view, bool resize, std::uint32_t edges)
{
    if (locked() || view->surface() != m_seat->keyboard_state.focused_surface)
        return;

    m_grabbed_view = view;
    m_cursor_mode = resize ? CursorMode::Resize : CursorMode::Move;
    wlr_seat_pointer_notify_clear_focus(m_seat);

    if (!resize) {
        m_grab_x = m_cursor->x - view->x();
        m_grab_y = m_cursor->y - view->y();
        return;
    }

    const wlr_box geo = view->toplevel()->base->geometry;
    const double border_x = view->x() + geo.x + ((edges & WLR_EDGE_RIGHT) ? geo.width : 0);
    const double border_y = view->y() + geo.y + ((edges & WLR_EDGE_BOTTOM) ? geo.height : 0);
    m_grab_x = m_cursor->x - border_x;
    m_grab_y = m_cursor->y - border_y;
    m_grab_geobox = geo;
    m_grab_geobox.x += view->x();
    m_grab_geobox.y += view->y();
    m_resize_edges = edges;
}

void Server::reset_cursor_mode()
{
    m_cursor_mode = CursorMode::Passthrough;
    m_grabbed_view = nullptr;
}

void Server::process_cursor_move()
{
    m_grabbed_view->move_to(static_cast<int>(m_cursor->x - m_grab_x), static_cast<int>(m_cursor->y - m_grab_y));
}

void Server::process_cursor_resize()
{
    View* view = m_grabbed_view;
    const double border_x = m_cursor->x - m_grab_x;
    const double border_y = m_cursor->y - m_grab_y;

    int left = m_grab_geobox.x;
    int right = m_grab_geobox.x + m_grab_geobox.width;
    int top = m_grab_geobox.y;
    int bottom = m_grab_geobox.y + m_grab_geobox.height;

    if (m_resize_edges & WLR_EDGE_TOP)
        top = std::min(static_cast<int>(border_y), bottom - 1);
    else if (m_resize_edges & WLR_EDGE_BOTTOM)
        bottom = std::max(static_cast<int>(border_y), top + 1);
    if (m_resize_edges & WLR_EDGE_LEFT)
        left = std::min(static_cast<int>(border_x), right - 1);
    else if (m_resize_edges & WLR_EDGE_RIGHT)
        right = std::max(static_cast<int>(border_x), left + 1);

    const wlr_box geo = view->toplevel()->base->geometry;
    view->move_to(left - geo.x, top - geo.y);
    wlr_xdg_toplevel_set_size(view->toplevel(), right - left, bottom - top);
}

} // namespace wfa
