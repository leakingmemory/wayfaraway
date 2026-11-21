#pragma once

#include <functional>
#include <string>
#include <vector>

#include "double_click.hpp"
#include "wlr.hpp"

typedef struct _cairo_surface cairo_surface_t;

namespace wfa {

// Minimized windows, shown as captioned icons along the bottom of the
// primary monitor. Double-clicking one restores it.
class DesktopIcons {
public:
    struct Item {
        const void* key;               // identifies the window
        std::string label;
        cairo_surface_t* icon;         // not owned
        std::function<void()> restore; // must call remove(key)
    };

    explicit DesktopIcons(wlr_scene_tree* parent);
    ~DesktopIcons();

    DesktopIcons(const DesktopIcons&) = delete;
    DesktopIcons& operator=(const DesktopIcons&) = delete;

    wlr_scene_tree* tree() const { return m_tree; }

    void add(Item item);
    void remove(const void* key);
    void set_label(const void* key, const std::string& label);
    void set_area(wlr_box area);

    // A press at a layout point inside the icon layer.
    void press(double lx, double ly, std::uint32_t time_msec);

private:
    struct Entry {
        Item item;
        wlr_scene_buffer* node;
        wlr_box box{};
    };

    void relayout();
    void paint(Entry& entry);

    wlr_scene_tree* m_tree;
    std::vector<Entry> m_entries;
    wlr_box m_area{};
    const void* m_selected = nullptr;
    DoubleClick m_clicks;
};

} // namespace wfa
