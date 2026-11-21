#include <algorithm>
#include <chrono>
#include <span>

#include <linux/input-event-codes.h>

#include "server.hpp"
#include "view.hpp"

namespace wfa {

namespace {

constexpr wlr_keyboard_impl keyboard_impl = {
    .name = "rdp-keyboard",
    .led_update = nullptr,
};

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
    wlr_keyboard_init(&m_keyboard, &keyboard_impl, "rdp-keyboard");

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
    m_keyboard_modifiers.connect(&m_keyboard.events.modifiers, [this](void*) {
        if (!locked())
            wlr_seat_keyboard_notify_modifiers(m_seat, &m_keyboard.modifiers);
    });
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
            m_lock_screen->handle_key(sym, std::string_view(utf8, std::clamp(len, 0, static_cast<int>(sizeof(utf8) - 1))));
            explicit_bzero(utf8, sizeof(utf8));
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

    if (!handled)
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
        if (!m_views.empty() && m_views.front()->mapped())
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
        if (m_cursor_mode != CursorMode::Passthrough) {
            reset_cursor_mode();
            return;
        }
        wlr_seat_pointer_notify_button(m_seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
        wlr_seat_pointer_notify_frame(m_seat);
        return;
    }

    wlr_surface* surface = nullptr;
    double sx = 0, sy = 0;
    View* view = view_at(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    if (view) {
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

    wlr_surface* surface = nullptr;
    double sx = 0, sy = 0;
    View* view = view_at(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    if (!view)
        wlr_cursor_set_xcursor(m_cursor, m_cursor_manager, "default");

    if (surface) {
        wlr_seat_pointer_notify_enter(m_seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(m_seat, time_msec, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(m_seat);
    }
    wlr_seat_pointer_notify_frame(m_seat);
}

View* Server::view_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy) const
{
    wlr_scene_node* node = wlr_scene_node_at(&m_desktop_tree->node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return nullptr;

    wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!scene_surface)
        return nullptr;
    *surface = scene_surface->surface;

    for (wlr_scene_tree* tree = node->parent; tree; tree = tree->node.parent) {
        if (tree->node.data)
            return static_cast<View*>(tree->node.data);
    }
    return nullptr;
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
