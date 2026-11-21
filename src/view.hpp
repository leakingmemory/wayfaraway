#pragma once

#include "listener.hpp"
#include "wlr.hpp"

namespace wfa {

class Server;

// One xdg toplevel window on the desktop.
class View {
public:
    View(Server& server, wlr_xdg_toplevel* toplevel);
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    wlr_xdg_toplevel* toplevel() const { return m_toplevel; }
    wlr_surface* surface() const { return m_toplevel->base->surface; }
    wlr_scene_tree* tree() const { return m_tree; }
    bool mapped() const { return m_mapped; }

    int x() const { return m_x; }
    int y() const { return m_y; }
    void move_to(int x, int y);

    void set_activated(bool activated);
    void close();

    // Fits a maximized/fullscreen view to the output after a resize.
    void fit_to_output();

private:
    void toggle_maximized();

    Server& m_server;
    wlr_xdg_toplevel* m_toplevel;
    wlr_scene_tree* m_tree;

    bool m_mapped = false;
    int m_x = 0;
    int m_y = 0;
    wlr_box m_saved_geometry{};

    Listener m_map;
    Listener m_unmap;
    Listener m_commit;
    Listener m_destroy;
    Listener m_request_move;
    Listener m_request_resize;
    Listener m_request_maximize;
    Listener m_request_fullscreen;
};

// Popups are owned by their wlr_xdg_popup and delete themselves on destroy.
class Popup {
public:
    explicit Popup(wlr_xdg_popup* popup);

private:
    wlr_xdg_popup* m_popup;
    Listener m_commit;
    Listener m_destroy;
};

} // namespace wfa
