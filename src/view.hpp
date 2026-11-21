#pragma once

#include <string>

#include "listener.hpp"
#include "retro_theme.hpp"
#include "wlr.hpp"

namespace wfa {

class Server;

// One xdg toplevel window on the desktop.
//
// Scene layout: m_root (positioned at the surface origin) holds the frame
// buffer, behind, and the xdg surface tree. The frame is only drawn once the
// client has accepted server-side decorations.
class View {
public:
    View(Server& server, wlr_xdg_toplevel* toplevel);
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    wlr_xdg_toplevel* toplevel() const { return m_toplevel; }
    wlr_surface* surface() const { return m_toplevel->base->surface; }
    // The node to raise, hide and hit-test.
    wlr_scene_tree* tree() const { return m_root; }
    bool mapped() const { return m_mapped; }
    std::string title() const { return m_toplevel->title ? m_toplevel->title : ""; }
    std::string app_id() const { return m_toplevel->app_id ? m_toplevel->app_id : ""; }

    // Position of the surface origin (not of the frame).
    int x() const { return m_x; }
    int y() const { return m_y; }
    void move_to(int x, int y);

    void set_activated(bool activated);
    void close();
    bool maximized() const { return m_toplevel->current.maximized; }
    void toggle_maximized();
    // Fits a maximized/fullscreen view to the output after a resize.
    void fit_to_output();

    bool minimized() const { return m_minimized; }
    void set_minimized(bool minimized);

    // --- Server-side decorations ---------------------------------------------
    void set_decoration(wlr_xdg_toplevel_decoration_v1* decoration);
    bool decorated() const { return m_decorated; }
    // The window geometry, in layout coordinates.
    wlr_box content_box() const;
    // The geometry plus the frame, if any.
    wlr_box outer_box() const;
    // What part of the frame is at a layout point (None if undecorated).
    retro::FrameHit frame_hit(double lx, double ly) const;
    static constexpr retro::FrameButtons frame_buttons{ true, true, true };

private:
    // The monitor this view is on: the one containing its centre, or, before
    // it is mapped, the one under the pointer (where it will be placed).
    wlr_box output_box() const;
    void apply_decoration_mode();
    void update_frame();

    Server& m_server;
    wlr_xdg_toplevel* m_toplevel;
    wlr_scene_tree* m_root;
    wlr_scene_buffer* m_frame;
    wlr_scene_tree* m_tree;

    bool m_mapped = false;
    bool m_minimized = false;
    bool m_activated = false;
    int m_x = 0;
    int m_y = 0;
    wlr_box m_saved_geometry{};

    wlr_xdg_toplevel_decoration_v1* m_decoration = nullptr;
    bool m_decorated = false;
    // What the frame buffer currently shows, to skip needless repaints.
    struct FrameState {
        int width = 0;
        int height = 0;
        bool active = false;
        bool maximized = false;
        std::string title;
        bool operator==(const FrameState&) const = default;
    } m_frame_state;

    Listener m_map;
    Listener m_unmap;
    Listener m_commit;
    Listener m_destroy;
    Listener m_request_move;
    Listener m_request_resize;
    Listener m_request_maximize;
    Listener m_request_fullscreen;
    Listener m_request_minimize;
    Listener m_set_title;
    Listener m_decoration_request_mode;
    Listener m_decoration_destroy;
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
