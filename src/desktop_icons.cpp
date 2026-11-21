#include "desktop_icons.hpp"

#include <algorithm>

#include "cairo_buffer.hpp"
#include "retro_theme.hpp"

namespace wfa {

namespace {

constexpr int cell_width = 76;
constexpr int cell_height = 64;
constexpr int icon_size = 32;
constexpr int spacing = 4;
constexpr int margin = 8;

} // namespace

DesktopIcons::DesktopIcons(wlr_scene_tree* parent)
    : m_tree(wlr_scene_tree_create(parent))
{
}

DesktopIcons::~DesktopIcons()
{
    wlr_scene_node_destroy(&m_tree->node);
}

void DesktopIcons::add(Item item)
{
    remove(item.key);
    Entry entry{ std::move(item), wlr_scene_buffer_create(m_tree, nullptr) };
    m_entries.push_back(std::move(entry));
    paint(m_entries.back());
    relayout();
}

void DesktopIcons::remove(const void* key)
{
    auto it = std::ranges::find_if(m_entries, [key](const Entry& e) { return e.item.key == key; });
    if (it == m_entries.end())
        return;
    wlr_scene_node_destroy(&it->node->node);
    m_entries.erase(it);
    if (m_selected == key)
        m_selected = nullptr;
    relayout();
}

void DesktopIcons::set_label(const void* key, const std::string& label)
{
    for (auto& entry : m_entries) {
        if (entry.item.key == key && entry.item.label != label) {
            entry.item.label = label;
            paint(entry);
        }
    }
}

void DesktopIcons::set_area(wlr_box area)
{
    m_area = area;
    relayout();
}

// Left to right along the bottom, wrapping upwards.
void DesktopIcons::relayout()
{
    int x = m_area.x + margin;
    int y = m_area.y + m_area.height - margin - cell_height;
    for (auto& entry : m_entries) {
        if (x + cell_width > m_area.x + m_area.width - margin && x != m_area.x + margin) {
            x = m_area.x + margin;
            y -= cell_height + spacing;
        }
        entry.box = { x, y, cell_width, cell_height };
        wlr_scene_node_set_position(&entry.node->node, x, y);
        x += cell_width + spacing;
    }
}

void DesktopIcons::paint(Entry& entry)
{
    const bool selected = entry.item.key == m_selected;
    CairoBuffer::show(entry.node, cell_width, cell_height, [&](cairo_t* cr) {
        retro::paint_icon(cr, entry.item.icon, (cell_width - icon_size) / 2.0, 2, icon_size);
        retro::paint_label(cr, entry.item.label, cell_width / 2.0, icon_size + 6, cell_width - 4, selected, retro::white);
    });
}

void DesktopIcons::press(double lx, double ly, std::uint32_t time_msec)
{
    const auto it = std::ranges::find_if(m_entries, [&](const Entry& e) {
        return lx >= e.box.x && lx < e.box.x + e.box.width && ly >= e.box.y && ly < e.box.y + e.box.height;
    });
    const void* key = it == m_entries.end() ? nullptr : it->item.key;

    if (key != m_selected) {
        m_selected = key;
        for (auto& entry : m_entries)
            paint(entry);
    }
    if (key && m_clicks.click(key, 0, time_msec, lx, ly)) {
        // restore() removes the entry, so do not touch `it` afterwards.
        auto restore = it->item.restore;
        if (restore)
            restore();
    }
}

} // namespace wfa
