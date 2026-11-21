#include "server.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>

#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rdp_server.hpp"
#include "view.hpp"

namespace wfa {

namespace {

std::string current_user()
{
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_name)
        return pw->pw_name;
    if (const char* user = std::getenv("USER"))
        return user;
    return "unknown";
}

} // namespace

Server::Server(Config config)
    : m_config(std::move(config))
{
    m_display = wl_display_create();
    m_loop = wl_display_get_event_loop(m_display);

    m_backend = wlr_headless_backend_create(m_loop);
    // A CPU renderer keeps every frame directly readable for the RDP encoder.
    m_renderer = wlr_pixman_renderer_create();
    wlr_renderer_init_wl_display(m_renderer, m_display);
    m_allocator = wlr_allocator_autocreate(m_backend, m_renderer);

    m_compositor = wlr_compositor_create(m_display, 6, m_renderer);
    wlr_subcompositor_create(m_display);
    wlr_data_device_manager_create(m_display);
    wlr_primary_selection_v1_device_manager_create(m_display);
    wlr_viewporter_create(m_display);

    m_output_layout = wlr_output_layout_create(m_display);
    wlr_xdg_output_manager_v1_create(m_display, m_output_layout);

    m_scene = wlr_scene_create();
    m_scene_layout = wlr_scene_attach_output_layout(m_scene, m_output_layout);

    static constexpr float background_color[4] = { 0.10f, 0.11f, 0.15f, 1.0f };
    m_background = wlr_scene_rect_create(&m_scene->tree, 0, 0, background_color);
    m_desktop_tree = wlr_scene_tree_create(&m_scene->tree);
    m_lock_tree = wlr_scene_tree_create(&m_scene->tree);
    // Fail closed: nothing on the desktop is visible until the first unlock.
    wlr_scene_node_set_enabled(&m_desktop_tree->node, false);

    m_xdg_shell = wlr_xdg_shell_create(m_display, 6);
    m_new_toplevel.connect(&m_xdg_shell->events.new_toplevel, [this](void* data) {
        m_views.push_front(std::make_unique<View>(*this, static_cast<wlr_xdg_toplevel*>(data)));
    });
    m_new_popup.connect(&m_xdg_shell->events.new_popup, [](void* data) {
        new Popup(static_cast<wlr_xdg_popup*>(data));
    });

    m_cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(m_cursor, m_output_layout);
    m_cursor_manager = wlr_xcursor_manager_create(nullptr, 24);

    m_seat = wlr_seat_create(m_display, "seat0");
    wlr_seat_set_capabilities(m_seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    setup_keyboard();

    m_request_set_cursor.connect(&m_seat->events.request_set_cursor, [this](void* data) {
        auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
        if (!locked() && event->seat_client == m_seat->pointer_state.focused_client)
            wlr_cursor_set_surface(m_cursor, event->surface, event->hotspot_x, event->hotspot_y);
    });
    m_request_set_selection.connect(&m_seat->events.request_set_selection, [this](void* data) {
        auto* event = static_cast<wlr_seat_request_set_selection_event*>(data);
        wlr_seat_set_selection(m_seat, event->source, event->serial);
    });
    m_request_set_primary_selection.connect(&m_seat->events.request_set_primary_selection, [this](void* data) {
        auto* event = static_cast<wlr_seat_request_set_primary_selection_event*>(data);
        wlr_seat_set_primary_selection(m_seat, event->source, event->serial);
    });

    m_authenticator = std::make_unique<Authenticator>(m_loop, m_config.pam_service, current_user());
    m_lock_screen = std::make_unique<LockScreen>(m_loop, m_lock_tree, *m_authenticator);
    m_lock_screen->on_unlock = [this] { on_unlocked(); };

    m_new_output.connect(&m_backend->events.new_output, [this](void* data) {
        create_output(static_cast<wlr_output*>(data));
    });
}

Server::~Server()
{
    set_frame_sink(nullptr);
    m_rdp.reset();
    if (m_display)
        wl_display_destroy_clients(m_display);
    m_views.clear();

    m_lock_screen.reset();
    m_authenticator.reset();

    m_new_toplevel.disconnect();
    m_new_popup.disconnect();
    m_request_set_cursor.disconnect();
    m_request_set_selection.disconnect();
    m_request_set_primary_selection.disconnect();
    m_keyboard_key.disconnect();
    m_keyboard_modifiers.disconnect();
    m_new_output.disconnect();

    wlr_scene_node_destroy(&m_scene->tree.node);
    wlr_xcursor_manager_destroy(m_cursor_manager);
    wlr_cursor_destroy(m_cursor);
    wlr_keyboard_finish(&m_keyboard);
    wlr_backend_destroy(m_backend);
    wl_display_destroy(m_display);
    wlr_allocator_destroy(m_allocator);
    wlr_renderer_destroy(m_renderer);
}

bool Server::start()
{
    const char* socket = wl_display_add_socket_auto(m_display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Unable to create a Wayland socket");
        return false;
    }

    m_rdp = std::make_unique<RdpServer>(*this);
    set_frame_sink(m_rdp.get());

    if (!wlr_backend_start(m_backend)) {
        wlr_log(WLR_ERROR, "Unable to start the headless backend");
        return false;
    }
    // One output until an RDP client says how many monitors it has.
    set_monitor_layout({ { 0, 0, m_config.width, m_config.height, true } });
    if (m_outputs.empty()) {
        wlr_log(WLR_ERROR, "Unable to create the virtual output");
        return false;
    }
    wlr_cursor_set_xcursor(m_cursor, m_cursor_manager, "default");
    const wlr_box primary = primary_box();
    wlr_cursor_warp_closest(m_cursor, nullptr, primary.x + primary.width / 2.0, primary.y + primary.height / 2.0);

    if (!m_rdp->listen())
        return false;

    setenv("WAYLAND_DISPLAY", socket, 1);
    // Clients must never try to talk to whatever display we might have been
    // started from.
    unsetenv("DISPLAY");
    wlr_log(WLR_INFO, "Wayland socket: %s; desktop is locked", socket);

    for (const auto& command : m_config.startup_commands)
        spawn(command);
    return true;
}

void Server::run()
{
    wl_display_run(m_display);
}

void Server::lock()
{
    if (locked()) {
        m_lock_screen->lock(); // resets any half-typed password
        return;
    }

    wlr_log(WLR_INFO, "Locking session");
    reset_cursor_mode();
    m_lock_screen->lock();
    wlr_scene_node_set_enabled(&m_desktop_tree->node, false);
    wlr_seat_keyboard_notify_clear_focus(m_seat);
    wlr_seat_pointer_notify_clear_focus(m_seat);
    wlr_cursor_set_xcursor(m_cursor, m_cursor_manager, "default");
    if (on_lock_changed)
        on_lock_changed(true);
}

void Server::on_unlocked()
{
    wlr_scene_node_set_enabled(&m_desktop_tree->node, true);
    if (on_lock_changed)
        on_lock_changed(false);
    auto it = std::ranges::find_if(m_views, [](const auto& view) { return view->mapped(); });
    if (it != m_views.end())
        focus_view(it->get());
    process_cursor_motion(0);
}

void Server::create_output(wlr_output* wlr_output)
{
    // Only outputs we asked for exist (headless backend); the monitor they
    // stand for was stashed by set_monitor_layout.
    const Monitor monitor = m_pending_monitor;

    wlr_output_init_render(wlr_output, m_allocator, m_renderer);

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, monitor.width, monitor.height, 0);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    auto output = std::make_unique<Output>();
    Output* raw = output.get();
    raw->output = wlr_output;
    raw->frame.connect(&wlr_output->events.frame, [this, raw](void*) { render_frame(*raw); });
    raw->destroy.connect(&wlr_output->events.destroy, [this, raw](void*) {
        // Deletes `raw`, including the listener running this.
        std::erase_if(m_outputs, [raw](const auto& o) { return o.get() == raw; });
    });

    wlr_output_layout_output* layout_output = wlr_output_layout_add(m_output_layout, wlr_output, monitor.x, monitor.y);
    raw->scene_output = wlr_scene_output_create(m_scene, wlr_output);
    wlr_scene_output_layout_add_output(m_scene_layout, layout_output, raw->scene_output);

    m_outputs.push_back(std::move(output));
}

void Server::configure_output(Output& output, const Monitor& monitor)
{
    if (output.output->width != monitor.width || output.output->height != monitor.height) {
        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_custom_mode(&state, monitor.width, monitor.height, 0);
        if (!wlr_output_commit_state(output.output, &state))
            wlr_log(WLR_ERROR, "Failed to resize %s to %dx%d", output.output->name, monitor.width, monitor.height);
        wlr_output_state_finish(&state);
    }
    // Moves the output if it is already in the layout.
    wlr_output_layout_add(m_output_layout, output.output, monitor.x, monitor.y);
}

std::vector<Monitor> Server::set_monitor_layout(std::vector<Monitor> monitors)
{
    constexpr std::size_t max_monitors = 16;
    constexpr int min_size = 200;
    constexpr int max_size = 8192;

    if (monitors.empty())
        monitors.push_back({ 0, 0, m_config.width, m_config.height, true });
    if (monitors.size() > max_monitors)
        monitors.resize(max_monitors);

    for (auto& m : monitors) {
        m.width = std::clamp(m.width, min_size, max_size);
        m.height = std::clamp(m.height, min_size, max_size);
    }

    // Exactly one primary, and it comes first.
    auto primary = std::ranges::find_if(monitors, &Monitor::primary);
    if (primary == monitors.end())
        primary = monitors.begin();
    std::rotate(monitors.begin(), primary, primary + 1);
    for (std::size_t i = 0; i < monitors.size(); ++i)
        monitors[i].primary = i == 0;

    // Normalise so the desktop's top-left is (0, 0).
    const int min_x = std::ranges::min(monitors, {}, &Monitor::x).x;
    const int min_y = std::ranges::min(monitors, {}, &Monitor::y).y;
    int max_x = 0, max_y = 0;
    for (auto& m : monitors) {
        m.x -= min_x;
        m.y -= min_y;
        max_x = std::max(max_x, m.x + m.width);
        max_y = std::max(max_y, m.y + m.height);
    }
    if (max_x > max_size || max_y > max_size) {
        wlr_log(WLR_ERROR, "Monitor layout %dx%d exceeds %dx%d; using the primary monitor only", max_x, max_y,
            max_size, max_size);
        monitors.resize(1);
        monitors[0].x = monitors[0].y = 0;
    }

    if (monitors == m_monitors && m_outputs.size() == monitors.size())
        return m_monitors;

    // Reuse existing outputs (keeping windows where they are), then add or
    // remove the difference.
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        if (i < m_outputs.size()) {
            configure_output(*m_outputs[i], monitors[i]);
        } else {
            m_pending_monitor = monitors[i];
            if (!wlr_headless_add_output(m_backend, monitors[i].width, monitors[i].height))
                wlr_log(WLR_ERROR, "Unable to create a virtual output");
        }
    }
    while (m_outputs.size() > monitors.size())
        wlr_output_destroy(m_outputs.back()->output); // erased by its destroy listener

    m_monitors = std::move(monitors);
    layout_changed();
    return m_monitors;
}

void Server::layout_changed()
{
    wlr_output_layout_get_box(m_output_layout, nullptr, &m_desktop);
    for (const auto& m : m_monitors)
        wlr_log(WLR_INFO, "Output %dx%d at %d,%d%s", m.width, m.height, m.x, m.y, m.primary ? " (primary)" : "");
    wlr_log(WLR_INFO, "Desktop is %dx%d", m_desktop.width, m_desktop.height);

    wlr_scene_rect_set_size(m_background, m_desktop.width, m_desktop.height);
    m_lock_screen->set_layout(m_desktop.width, m_desktop.height, primary_box());
    if (m_frame_sink)
        m_frame_sink->on_layout(m_desktop.width, m_desktop.height);

    // The sink starts from a blank desktop, so repaint every output in full.
    for (auto& output : m_outputs) {
        wlr_damage_ring_add_whole(&output->scene_output->damage_ring);
        wlr_output_schedule_frame(output->output);
    }

    // Windows left outside every output move to the primary one; maximized
    // windows follow the size of theirs.
    const wlr_box primary = primary_box();
    for (auto& view : m_views) {
        if (!view->mapped())
            continue;
        const wlr_box geometry = view->toplevel()->base->geometry;
        const wlr_box box{ view->x() + geometry.x, view->y() + geometry.y, geometry.width, geometry.height };
        if (!wlr_output_layout_intersects(m_output_layout, nullptr, &box)) {
            view->move_to(primary.x + std::max(0, (primary.width - geometry.width) / 2) - geometry.x,
                primary.y + std::max(0, (primary.height - geometry.height) / 2) - geometry.y);
        }
        view->fit_to_output();
    }

    // Keep the pointer on the desktop.
    wlr_cursor_warp_closest(m_cursor, nullptr, m_cursor->x, m_cursor->y);
}

wlr_box Server::output_box_at(double lx, double ly) const
{
    wlr_output* output = wlr_output_layout_output_at(m_output_layout, lx, ly);
    if (!output) {
        double cx = 0, cy = 0;
        wlr_output_layout_closest_point(m_output_layout, nullptr, lx, ly, &cx, &cy);
        output = wlr_output_layout_output_at(m_output_layout, cx, cy);
    }
    if (!output)
        return primary_box();
    wlr_box box{};
    wlr_output_layout_get_box(m_output_layout, output, &box);
    return box;
}

wlr_box Server::primary_box() const
{
    if (m_monitors.empty())
        return { 0, 0, m_config.width, m_config.height };
    const Monitor& m = m_monitors.front();
    return { m.x, m.y, m.width, m.height };
}

void Server::render_frame(Output& output)
{
    wlr_output_state state;
    wlr_output_state_init(&state);

    if (wlr_scene_output_build_state(output.scene_output, &state, nullptr)) {
        if (m_frame_sink && (state.committed & WLR_OUTPUT_STATE_BUFFER) && state.buffer) {
            void* data = nullptr;
            std::uint32_t format = 0;
            std::size_t stride = 0;
            if (wlr_buffer_begin_data_ptr_access(state.buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
                pixman_region32_t damage;
                if (state.committed & WLR_OUTPUT_STATE_DAMAGE) {
                    pixman_region32_init(&damage);
                    pixman_region32_copy(&damage, &state.damage);
                } else {
                    pixman_region32_init_rect(&damage, 0, 0, state.buffer->width, state.buffer->height);
                }

                wlr_box box{};
                wlr_output_layout_get_box(m_output_layout, output.output, &box);
                m_frame_sink->on_frame(static_cast<const std::byte*>(data), state.buffer->width,
                    state.buffer->height, stride, box.x, box.y, &damage);

                pixman_region32_fini(&damage);
                wlr_buffer_end_data_ptr_access(state.buffer);
            }
        }
        wlr_output_commit_state(output.output, &state);
    }
    wlr_output_state_finish(&state);

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output.scene_output, &now);
}

void Server::spawn(const std::string& command) const
{
    // Double fork so children are reparented to init and never become
    // zombies; we cannot ignore SIGCHLD because PAM helpers need waitpid().
    pid_t child = fork();
    if (child < 0) {
        wlr_log_errno(WLR_ERROR, "fork failed");
        return;
    }
    if (child == 0) {
        setsid();
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    waitpid(child, nullptr, 0);
}

// --- Views ---------------------------------------------------------------

void Server::focus_view(View* view)
{
    if (!view)
        return;

    auto it = std::ranges::find_if(m_views, [view](const auto& v) { return v.get() == view; });
    if (it == m_views.end())
        return;

    wlr_surface* previous = m_seat->keyboard_state.focused_surface;
    if (previous == view->surface() && !locked())
        return;

    if (previous) {
        if (auto* prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(previous))
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
    }

    m_views.splice(m_views.begin(), m_views, it);
    wlr_scene_node_raise_to_top(&view->tree()->node);
    view->set_activated(true);

    if (locked())
        return;

    wlr_seat_keyboard_notify_enter(m_seat, view->surface(), m_keyboard.keycodes, m_keyboard.num_keycodes,
        &m_keyboard.modifiers);
}

void Server::view_mapped(View* view)
{
    // Centre new windows on the monitor under the pointer, cascading so they
    // do not stack exactly.
    const wlr_box geometry = view->toplevel()->base->geometry;
    const wlr_box output = output_box_at(m_cursor->x, m_cursor->y);
    const int mapped = static_cast<int>(std::ranges::count_if(m_views, [](const auto& v) { return v->mapped(); }));
    const int offset = ((mapped - 1) % 8) * 32;
    view->move_to(output.x + std::max(0, (output.width - geometry.width) / 2 + offset) - geometry.x,
        output.y + std::max(0, (output.height - geometry.height) / 2 + offset) - geometry.y);
    focus_view(view);
}

void Server::view_unmapped(View* view)
{
    if (m_grabbed_view == view)
        reset_cursor_mode();

    if (m_seat->keyboard_state.focused_surface == view->surface()) {
        wlr_seat_keyboard_notify_clear_focus(m_seat);
        auto next = std::ranges::find_if(m_views, [view](const auto& v) { return v.get() != view && v->mapped(); });
        if (next != m_views.end())
            focus_view(next->get());
    }
}

void Server::view_destroyed(View* view)
{
    if (m_grabbed_view == view)
        reset_cursor_mode();
    std::erase_if(m_views, [view](const auto& v) { return v.get() == view; });
}

void Server::cycle_focus()
{
    // Bring the bottom-most mapped view to the top.
    auto it = std::find_if(m_views.rbegin(), m_views.rend(), [](const auto& v) { return v->mapped(); });
    if (it != m_views.rend())
        focus_view(it->get());
}

} // namespace wfa
