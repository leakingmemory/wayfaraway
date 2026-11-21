#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <set>
#include <vector>

#include "authenticator.hpp"
#include "config.hpp"
#include "desktop_entry.hpp"
#include "double_click.hpp"
#include "icon_loader.hpp"
#include "listener.hpp"
#include "lock_screen.hpp"
#include "retro_theme.hpp"
#include "wlr.hpp"

namespace wfa {

class View;
class RdpServer;
class ProgramManager;
class DesktopIcons;
class TaskSwitcher;

// One monitor of the desktop, in desktop coordinates.
struct Monitor {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool primary = false;

    bool operator==(const Monitor&) const = default;
};

// Receives every rendered frame of every virtual output.
class FrameSink {
public:
    virtual ~FrameSink() = default;
    // The desktop (the bounding box of all outputs, starting at 0,0) was
    // resized or rearranged. Every output re-renders in full afterwards.
    virtual void on_layout(int width, int height) = 0;
    // A frame of the output at (x, y) on the desktop. `damage` is in buffer
    // coordinates. Pixels are XRGB8888.
    virtual void on_frame(const std::byte* pixels, int width, int height, std::size_t stride, int x, int y,
        const pixman_region32_t* damage) = 0;
};

class Server {
public:
    explicit Server(Config config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start();
    void run();

    const Config& config() const { return m_config; }
    wl_event_loop* event_loop() const { return m_loop; }

    // --- Session -----------------------------------------------------------
    bool locked() const { return m_lock_screen->locked(); }
    void lock();
    // Called whenever the session locks or unlocks.
    std::function<void(bool locked)> on_lock_changed;

    // --- Outputs -----------------------------------------------------------
    void set_frame_sink(FrameSink* sink) { m_frame_sink = sink; }
    // Replaces the virtual outputs with one per monitor, reusing existing
    // ones. Coordinates may be negative; they are normalised so the desktop
    // starts at (0, 0). Returns the layout actually applied.
    std::vector<Monitor> set_monitor_layout(std::vector<Monitor> monitors);
    const std::vector<Monitor>& monitors() const { return m_monitors; }
    int desktop_width() const { return m_desktop.width; }
    int desktop_height() const { return m_desktop.height; }

    // --- Input injection (used by the RDP server) --------------------------
    void inject_key(std::uint32_t evdev_keycode, bool pressed);
    // Sets Caps/Num Lock to the client's toggle state (RDP synchronize event).
    void sync_lock_keys(bool caps_lock, bool num_lock);
    // Called with RDP KBD_SYNC_* flags whenever the keyboard LEDs change.
    std::function<void(std::uint32_t led_flags)> on_keyboard_leds;
    void inject_pointer_motion(double x, double y);
    void inject_pointer_button(std::uint32_t button, bool pressed);
    void inject_pointer_axis(wl_pointer_axis axis, double value, int value120);
    void release_all_input();

    // --- Window management (used by View) -----------------------------------
    wlr_scene_tree* desktop_tree() const { return m_desktop_tree; }
    wlr_seat* seat() const { return m_seat; }
    wlr_cursor* cursor() const { return m_cursor; }
    // The box of the output containing (or closest to) a desktop point.
    wlr_box output_box_at(double lx, double ly) const;
    wlr_box primary_box() const;

    void focus_view(View* view);
    void begin_interactive(View* view, bool resize, std::uint32_t edges);
    void view_mapped(View* view);
    void view_unmapped(View* view);
    void view_destroyed(View* view);
    void view_title_changed(View* view);
    void minimize_view(View* view);

    void spawn(const std::string& command) const;
    // Runs a program directly (no shell), optionally in `directory`.
    void spawn(const std::vector<std::string>& argv, const std::string& directory = {}) const;

private:
    enum class CursorMode { Passthrough, Move, Resize };

    // What is under the pointer.
    struct Hit {
        enum class Kind { None, View, Programs, Icons } kind = Kind::None;
        View* view = nullptr;
        wlr_surface* surface = nullptr; // null on a window frame
        double sx = 0, sy = 0;
    };
    Hit hit_test(double lx, double ly) const;

    // --- Desktop shell (launcher, minimized icons, frames) -------------------
    void reload_applications();
    void launch(const DesktopEntry& entry);
    void restore_view(View* view);
    void show_programs();
    void minimize_programs();
    void focus_programs();
    void unfocus_programs();
    void focus_topmost();
    bool focus_topmost_window();
    void toggle_programs();

    // Alt+Tab.
    void switcher_step(int direction);
    void switcher_commit();
    void switcher_cancel();
    void activate_task(const void* key);
    View* view_for_surface(wlr_surface* surface) const;
    cairo_surface_t* icon_for_view(const View& view);
    void frame_press(View* view, retro::FrameHit hit, std::uint32_t time_msec);
    void frame_release(double lx, double ly);
    void update_cursor_image(const Hit& hit);

    struct Output {
        wlr_output* output = nullptr;
        wlr_scene_output* scene_output = nullptr;
        Listener frame;
        Listener destroy;
    };

    void create_output(wlr_output* output);
    void configure_output(Output& output, const Monitor& monitor);
    void layout_changed();
    void render_frame(Output& output);

    void setup_keyboard();
    void on_key(const wlr_keyboard_key_event& event);
    void on_modifiers();
    static void on_led_update(wlr_keyboard* keyboard, std::uint32_t leds);
    bool handle_keybinding(xkb_keysym_t sym, std::uint32_t modifiers);
    void on_unlocked();

    void process_cursor_motion(std::uint32_t time_msec);
    void process_cursor_move();
    void process_cursor_resize();
    void reset_cursor_mode();

    Config m_config;

    wl_display* m_display = nullptr;
    wl_event_loop* m_loop = nullptr;
    wlr_backend* m_backend = nullptr;
    wlr_renderer* m_renderer = nullptr;
    wlr_allocator* m_allocator = nullptr;
    wlr_compositor* m_compositor = nullptr;
    wlr_output_layout* m_output_layout = nullptr;
    wlr_scene* m_scene = nullptr;
    wlr_scene_output_layout* m_scene_layout = nullptr;
    wlr_xdg_shell* m_xdg_shell = nullptr;
    wlr_seat* m_seat = nullptr;
    wlr_cursor* m_cursor = nullptr;
    wlr_xcursor_manager* m_cursor_manager = nullptr;

    // Same order as m_monitors; the primary monitor comes first.
    std::vector<std::unique_ptr<Output>> m_outputs;
    std::vector<Monitor> m_monitors;
    wlr_box m_desktop{};
    // Monitor being created by wlr_headless_add_output (see create_output).
    Monitor m_pending_monitor;

    // Scene layers, bottom to top.
    wlr_scene_rect* m_background = nullptr;
    wlr_scene_tree* m_desktop_tree = nullptr;
    wlr_scene_tree* m_lock_tree = nullptr;

    wlr_xdg_decoration_manager_v1* m_decoration_manager = nullptr;

    IconLoader m_icon_loader;
    std::vector<DesktopEntry> m_applications;
    std::unique_ptr<DesktopEntryWatcher> m_application_watcher;
    // Bottom of the desktop tree, below every window.
    std::unique_ptr<DesktopIcons> m_desktop_icons;
    std::unique_ptr<ProgramManager> m_programs;
    bool m_programs_focused = false;
    // Between the desktop and the lock screen.
    wlr_scene_tree* m_overlay_tree = nullptr;
    std::unique_ptr<TaskSwitcher> m_switcher;
    // Super pressed on its own; released without another key, it toggles
    // the launcher.
    bool m_super_tap = false;
    bool m_programs_grab = false;

    // A frame button press waiting for its release.
    View* m_frame_view = nullptr;
    retro::FramePart m_frame_part = retro::FramePart::None;
    DoubleClick m_frame_clicks;
    const char* m_cursor_image = nullptr;

    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<LockScreen> m_lock_screen;
    std::unique_ptr<RdpServer> m_rdp;
    FrameSink* m_frame_sink = nullptr;

    // Front is the top of the stack / focused view.
    std::list<std::unique_ptr<View>> m_views;

    wlr_keyboard m_keyboard{};
    std::set<std::uint32_t> m_pressed_buttons;

    CursorMode m_cursor_mode = CursorMode::Passthrough;
    View* m_grabbed_view = nullptr;
    double m_grab_x = 0, m_grab_y = 0;
    wlr_box m_grab_geobox{};
    std::uint32_t m_resize_edges = 0;

    Listener m_new_output;
    Listener m_new_toplevel;
    Listener m_new_popup;
    Listener m_new_decoration;
    Listener m_keyboard_key;
    Listener m_keyboard_modifiers;
    Listener m_request_set_cursor;
    Listener m_request_set_selection;
    Listener m_request_set_primary_selection;
};

} // namespace wfa
