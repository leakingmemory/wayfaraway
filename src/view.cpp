#include "view.hpp"

#include "cairo_buffer.hpp"
#include "server.hpp"

namespace wfa {

View::View(Server& server, wlr_xdg_toplevel* toplevel)
    : m_server(server)
    , m_toplevel(toplevel)
    , m_root(wlr_scene_tree_create(server.desktop_tree()))
    , m_frame(wlr_scene_buffer_create(m_root, nullptr))
    , m_tree(wlr_scene_xdg_surface_create(m_root, toplevel->base))
{
    // Hit-testing walks up to the node carrying the View.
    m_root->node.data = this;
    wlr_scene_node_set_enabled(&m_frame->node, false);
    // Popups look up their parent's scene tree through this.
    toplevel->base->data = m_tree;

    wlr_surface* surface = toplevel->base->surface;

    m_map.connect(&surface->events.map, [this](void*) {
        m_mapped = true;
        update_frame();
        m_server.view_mapped(this);
    });
    m_unmap.connect(&surface->events.unmap, [this](void*) {
        m_mapped = false;
        m_server.view_unmapped(this);
    });
    m_commit.connect(&surface->events.commit, [this](void*) {
        if (m_toplevel->base->initial_commit) {
            // Let the client pick its own size, but tell it how much room
            // there is.
            const wlr_box box = output_box();
            wlr_xdg_toplevel_set_bounds(m_toplevel, box.width - retro::frame_left - retro::frame_right,
                box.height - retro::frame_top - retro::frame_bottom);
            wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
            // A decoration object may have arrived before we could answer it.
            apply_decoration_mode();
        }
        // Geometry, maximized state or decoration mode may have changed.
        update_frame();
    });
    m_destroy.connect(&toplevel->events.destroy, [this](void*) {
        // Deletes `this`.
        m_server.view_destroyed(this);
    });

    m_request_move.connect(&toplevel->events.request_move, [this](void*) {
        m_server.begin_interactive(this, false, 0);
    });
    m_request_resize.connect(&toplevel->events.request_resize, [this](void* data) {
        auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
        m_server.begin_interactive(this, true, event->edges);
    });
    m_request_maximize.connect(&toplevel->events.request_maximize, [this](void*) {
        if (m_toplevel->base->initialized)
            toggle_maximized();
    });
    m_request_fullscreen.connect(&toplevel->events.request_fullscreen, [this](void*) {
        // Without a second output, fullscreen is treated like maximize.
        if (m_toplevel->base->initialized)
            toggle_maximized();
    });
    m_request_minimize.connect(&toplevel->events.request_minimize, [this](void*) {
        if (m_mapped)
            m_server.minimize_view(this);
    });
    m_set_title.connect(&toplevel->events.set_title, [this](void*) {
        update_frame();
        m_server.view_title_changed(this);
    });
}

View::~View() = default;

void View::move_to(int x, int y)
{
    m_x = x;
    m_y = y;
    wlr_scene_node_set_position(&m_root->node, x, y);
}

void View::set_activated(bool activated)
{
    m_activated = activated;
    wlr_xdg_toplevel_set_activated(m_toplevel, activated);
    update_frame();
}

void View::close()
{
    wlr_xdg_toplevel_send_close(m_toplevel);
}

void View::set_minimized(bool minimized)
{
    m_minimized = minimized;
    wlr_scene_node_set_enabled(&m_root->node, !minimized);
}

void View::toggle_maximized()
{
    const bool maximize = !m_toplevel->current.maximized;
    const wlr_box geometry = m_toplevel->base->geometry;
    if (maximize) {
        m_saved_geometry = { m_x, m_y, geometry.width, geometry.height };
        // The frame, if any, goes at the output edge; the content inside it.
        const wlr_box box = output_box();
        const int left = m_decorated ? retro::frame_left : 0;
        const int top = m_decorated ? retro::frame_top : 0;
        const int extra_w = m_decorated ? retro::frame_left + retro::frame_right : 0;
        const int extra_h = m_decorated ? retro::frame_top + retro::frame_bottom : 0;
        move_to(box.x + left - geometry.x, box.y + top - geometry.y);
        wlr_xdg_toplevel_set_size(m_toplevel, box.width - extra_w, box.height - extra_h);
    } else {
        move_to(m_saved_geometry.x, m_saved_geometry.y);
        wlr_xdg_toplevel_set_size(m_toplevel, m_saved_geometry.width, m_saved_geometry.height);
    }
    wlr_xdg_toplevel_set_maximized(m_toplevel, maximize);
}

wlr_box View::output_box() const
{
    wlr_cursor* cursor = m_server.cursor();
    if (!m_mapped)
        return m_server.output_box_at(cursor->x, cursor->y);
    const wlr_box content = content_box();
    return m_server.output_box_at(content.x + content.width / 2.0, content.y + content.height / 2.0);
}

void View::fit_to_output()
{
    if (!m_toplevel->base->initialized)
        return;

    const wlr_box box = output_box();
    const int extra_w = m_decorated ? retro::frame_left + retro::frame_right : 0;
    const int extra_h = m_decorated ? retro::frame_top + retro::frame_bottom : 0;
    wlr_xdg_toplevel_set_bounds(m_toplevel, box.width - extra_w, box.height - extra_h);
    if (m_toplevel->current.maximized) {
        const wlr_box geometry = m_toplevel->base->geometry;
        move_to(box.x + (m_decorated ? retro::frame_left : 0) - geometry.x,
            box.y + (m_decorated ? retro::frame_top : 0) - geometry.y);
        wlr_xdg_toplevel_set_size(m_toplevel, box.width - extra_w, box.height - extra_h);
    }
}

// --- Decorations ------------------------------------------------------------------

void View::set_decoration(wlr_xdg_toplevel_decoration_v1* decoration)
{
    m_decoration = decoration;
    m_decoration_request_mode.connect(&decoration->events.request_mode, [this](void*) { apply_decoration_mode(); });
    m_decoration_destroy.connect(&decoration->events.destroy, [this](void*) {
        m_decoration_request_mode.disconnect();
        m_decoration_destroy.disconnect();
        m_decoration = nullptr;
        update_frame();
    });
    apply_decoration_mode();
}

void View::apply_decoration_mode()
{
    // Whatever the client prefers: the frame is the look. Setting a mode
    // before the surface is initialised is a protocol error, so the initial
    // commit calls this again.
    if (m_decoration && m_toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(m_decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

wlr_box View::content_box() const
{
    const wlr_box geometry = m_toplevel->base->geometry;
    return { m_x + geometry.x, m_y + geometry.y, geometry.width, geometry.height };
}

wlr_box View::outer_box() const
{
    wlr_box box = content_box();
    if (m_decorated) {
        box.x -= retro::frame_left;
        box.y -= retro::frame_top;
        box.width += retro::frame_left + retro::frame_right;
        box.height += retro::frame_top + retro::frame_bottom;
    }
    return box;
}

retro::FrameHit View::frame_hit(double lx, double ly) const
{
    if (!m_decorated)
        return {};
    const wlr_box outer = outer_box();
    return retro::hit_test_frame(outer.width, outer.height, lx - outer.x, ly - outer.y, frame_buttons, !maximized());
}

void View::update_frame()
{
    // Only once the client has acknowledged server-side mode, so it is
    // never framed twice.
    m_decorated = m_decoration && m_decoration->current.mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    if (!m_decorated || !m_mapped) {
        wlr_scene_node_set_enabled(&m_frame->node, false);
        return;
    }

    const wlr_box geometry = m_toplevel->base->geometry;
    FrameState state{
        .width = geometry.width + retro::frame_left + retro::frame_right,
        .height = geometry.height + retro::frame_top + retro::frame_bottom,
        .active = m_activated,
        .maximized = maximized(),
        .title = title(),
    };
    if (state != m_frame_state) {
        CairoBuffer::show(m_frame, state.width, state.height, [&](cairo_t* cr) {
            retro::paint_frame(cr, state.width, state.height, state.title, state.active, state.maximized, frame_buttons);
        });
        m_frame_state = std::move(state);
    }
    wlr_scene_node_set_position(&m_frame->node, geometry.x - retro::frame_left, geometry.y - retro::frame_top);
    wlr_scene_node_set_enabled(&m_frame->node, true);
}

Popup::Popup(wlr_xdg_popup* popup)
    : m_popup(popup)
{
    wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (parent && parent->data) {
        auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
        popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);
    }

    m_commit.connect(&popup->base->surface->events.commit, [this](void*) {
        if (m_popup->base->initial_commit)
            wlr_xdg_surface_schedule_configure(m_popup->base);
    });
    m_destroy.connect(&popup->events.destroy, [this](void*) { delete this; });
}

} // namespace wfa
