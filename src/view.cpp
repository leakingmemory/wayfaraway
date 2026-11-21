#include "view.hpp"

#include "server.hpp"

namespace wfa {

View::View(Server& server, wlr_xdg_toplevel* toplevel)
    : m_server(server)
    , m_toplevel(toplevel)
    , m_tree(wlr_scene_xdg_surface_create(server.desktop_tree(), toplevel->base))
{
    m_tree->node.data = this;
    // Popups look up their parent's scene tree through this.
    toplevel->base->data = m_tree;

    wlr_surface* surface = toplevel->base->surface;

    m_map.connect(&surface->events.map, [this](void*) {
        m_mapped = true;
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
            wlr_xdg_toplevel_set_bounds(m_toplevel, box.width, box.height);
            wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
        }
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
}

View::~View() = default;

void View::move_to(int x, int y)
{
    m_x = x;
    m_y = y;
    wlr_scene_node_set_position(&m_tree->node, x, y);
}

void View::set_activated(bool activated)
{
    wlr_xdg_toplevel_set_activated(m_toplevel, activated);
}

void View::close()
{
    wlr_xdg_toplevel_send_close(m_toplevel);
}

void View::toggle_maximized()
{
    const bool maximize = !m_toplevel->current.maximized;
    if (maximize) {
        m_saved_geometry = { m_x, m_y, m_toplevel->base->geometry.width, m_toplevel->base->geometry.height };
        const wlr_box box = output_box();
        move_to(box.x - m_toplevel->base->geometry.x, box.y - m_toplevel->base->geometry.y);
        wlr_xdg_toplevel_set_size(m_toplevel, box.width, box.height);
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
    const wlr_box geometry = m_toplevel->base->geometry;
    return m_server.output_box_at(m_x + geometry.x + geometry.width / 2.0, m_y + geometry.y + geometry.height / 2.0);
}

void View::fit_to_output()
{
    if (!m_toplevel->base->initialized)
        return;

    const wlr_box box = output_box();
    wlr_xdg_toplevel_set_bounds(m_toplevel, box.width, box.height);
    if (m_toplevel->current.maximized) {
        move_to(box.x - m_toplevel->base->geometry.x, box.y - m_toplevel->base->geometry.y);
        wlr_xdg_toplevel_set_size(m_toplevel, box.width, box.height);
    }
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
