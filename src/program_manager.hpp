#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "desktop_entry.hpp"
#include "double_click.hpp"
#include "wlr.hpp"

namespace wfa {

class IconLoader;

// The launcher: a compositor-drawn window in the style of a classic program
// manager. Applications from .desktop files are sorted into groups by
// category. One group is open as an inner window showing its programs as
// icons; the others are group icons along the bottom.
//
// It stacks with application windows (raise() brings it to the front), but
// is not a Wayland client; the Server routes pointer and key input to it.
class ProgramManager {
public:
    using Launch = std::function<void(const DesktopEntry&)>;

    ProgramManager(wlr_scene_tree* parent, IconLoader& icons, Launch launch);
    ~ProgramManager();

    ProgramManager(const ProgramManager&) = delete;
    ProgramManager& operator=(const ProgramManager&) = delete;

    wlr_scene_tree* tree() const { return m_tree; }

    void set_entries(std::vector<DesktopEntry> entries);

    // Sizes and centres the window on `area` the first time, and afterwards
    // only keeps it on screen.
    void place(wlr_box area);

    bool visible() const { return m_visible; }
    void set_visible(bool visible);
    void set_active(bool active);
    void raise();

    // Pointer input in layout coordinates. press() returns true when it
    // starts a drag (moving the window); motion and release then follow.
    bool press(double lx, double ly, std::uint32_t time_msec);
    void motion(double lx, double ly);
    void release(double lx, double ly);
    void scroll(double lx, double ly, double delta);
    void key(xkb_keysym_t sym);

    std::function<void()> on_minimize;
    std::function<void()> on_focus; // clicked: wants keyboard focus

private:
    struct Group {
        std::string name;
        std::vector<const DesktopEntry*> programs;
        int scroll = 0;
        int selected = -1;
    };

    // Layout in window-local coordinates.
    struct Layout {
        wlr_box client;       // inside the outer frame
        wlr_box group;        // the open group window (outer)
        wlr_box programs;     // its client area, minus any scrollbar
        wlr_box scrollbar;    // empty if not needed
        wlr_box group_icons;  // the row(s) of other groups
        int columns = 1;
        int content_height = 0;
    };

    Layout layout() const;
    std::vector<std::size_t> other_groups() const;
    void render();
    void clamp_scroll();
    void scroll_to_selected();
    void open_group(std::size_t index);
    void launch_selected();

    IconLoader& m_icons;
    Launch m_launch;

    wlr_scene_tree* m_tree;
    wlr_scene_buffer* m_buffer;

    std::vector<DesktopEntry> m_entries;
    std::vector<Group> m_groups;
    std::size_t m_current = 0;
    int m_selected_group_icon = -1;

    int m_x = 0;
    int m_y = 0;
    int m_width = 600;
    int m_height = 440;
    bool m_placed = false;
    bool m_visible = true;
    bool m_active = false;

    bool m_dragging = false;
    double m_drag_x = 0;
    double m_drag_y = 0;
    DoubleClick m_clicks;
};

} // namespace wfa
