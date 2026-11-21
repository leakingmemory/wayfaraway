#include "program_manager.hpp"

#include <algorithm>
#include <map>

#include "cairo_buffer.hpp"
#include "icon_loader.hpp"
#include "retro_theme.hpp"

namespace wfa {

namespace {

constexpr const char* window_title = "Programs";
constexpr retro::FrameButtons window_buttons{ true, true, false };
constexpr retro::FrameButtons group_buttons{ false, false, false };

constexpr int margin = 8;
constexpr int cell_width = 76;
constexpr int cell_height = 70;
constexpr int group_cell_height = 64;
constexpr int icon_size = 32;
constexpr int scrollbar_width = 16;

bool contains(const wlr_box& box, double x, double y)
{
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}

wlr_box inset_frame(const wlr_box& outer)
{
    return { outer.x + retro::frame_left, outer.y + retro::frame_top,
        outer.width - retro::frame_left - retro::frame_right, outer.height - retro::frame_top - retro::frame_bottom };
}

int ceil_div(int a, int b)
{
    return b > 0 ? (a + b - 1) / b : 0;
}

} // namespace

ProgramManager::ProgramManager(wlr_scene_tree* parent, IconLoader& icons, Launch launch)
    : m_icons(icons)
    , m_launch(std::move(launch))
    , m_tree(wlr_scene_tree_create(parent))
    , m_buffer(wlr_scene_buffer_create(m_tree, nullptr))
{
}

ProgramManager::~ProgramManager()
{
    wlr_scene_node_destroy(&m_tree->node);
}

void ProgramManager::set_entries(std::vector<DesktopEntry> entries)
{
    const std::string current = m_current < m_groups.size() ? m_groups[m_current].name : std::string{};

    m_entries = std::move(entries);
    std::map<std::string, Group> groups;
    for (const auto& entry : m_entries) {
        Group& group = groups[entry.group()];
        group.name = entry.group();
        group.programs.push_back(&entry); // m_entries is already sorted by name
    }

    // Alphabetical, with the catch-all group last.
    m_groups.clear();
    for (auto& [name, group] : groups) {
        if (name != "Other")
            m_groups.push_back(std::move(group));
    }
    if (auto other = groups.find("Other"); other != groups.end())
        m_groups.push_back(std::move(other->second));

    auto it = std::ranges::find_if(m_groups, [&](const Group& g) { return g.name == current; });
    m_current = it == m_groups.end() ? 0 : static_cast<std::size_t>(it - m_groups.begin());
    m_selected_group_icon = -1;
    m_clicks.reset();
    clamp_scroll();
    render();
}

void ProgramManager::place(wlr_box area)
{
    if (!m_placed) {
        m_width = std::clamp(area.width - 40, 320, 600);
        m_height = std::clamp(area.height - 60, 240, 440);
        m_x = area.x + (area.width - m_width) / 2;
        m_y = area.y + (area.height - m_height) / 3;
        m_placed = true;
    } else {
        // Keep enough of the title bar on screen to drag it back.
        m_x = std::clamp(m_x, area.x - m_width + 80, area.x + area.width - 80);
        m_y = std::clamp(m_y, area.y, area.y + area.height - retro::frame_top);
    }
    wlr_scene_node_set_position(&m_tree->node, m_x, m_y);
    render();
}

void ProgramManager::set_visible(bool visible)
{
    m_visible = visible;
    m_dragging = false;
    wlr_scene_node_set_enabled(&m_tree->node, visible);
}

void ProgramManager::set_active(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    render();
}

void ProgramManager::raise()
{
    wlr_scene_node_raise_to_top(&m_tree->node);
}

std::vector<std::size_t> ProgramManager::other_groups() const
{
    std::vector<std::size_t> others;
    for (std::size_t i = 0; i < m_groups.size(); ++i) {
        if (i != m_current)
            others.push_back(i);
    }
    return others;
}

ProgramManager::Layout ProgramManager::layout() const
{
    Layout l;
    l.client = inset_frame({ 0, 0, m_width, m_height });

    const int others = static_cast<int>(other_groups().size());
    const int per_row = std::max(1, (l.client.width - 2 * margin) / cell_width);
    const int icon_rows = ceil_div(others, per_row);
    const int icons_height = icon_rows * group_cell_height;

    l.group = { l.client.x + margin, l.client.y + margin, l.client.width - 2 * margin,
        l.client.height - 2 * margin - icons_height - (icon_rows ? margin / 2 : 0) };
    l.group_icons = { l.client.x + margin, l.group.y + l.group.height + margin / 2, l.client.width - 2 * margin,
        icons_height };

    const wlr_box inner = inset_frame(l.group);
    const int count = m_current < m_groups.size() ? static_cast<int>(m_groups[m_current].programs.size()) : 0;

    l.programs = inner;
    l.columns = std::max(1, inner.width / cell_width);
    l.content_height = ceil_div(count, l.columns) * cell_height + margin;
    if (l.content_height > inner.height) {
        // Too many to fit: make room for a scrollbar.
        l.scrollbar = { inner.x + inner.width - scrollbar_width, inner.y, scrollbar_width, inner.height };
        l.programs.width -= scrollbar_width;
        l.columns = std::max(1, l.programs.width / cell_width);
        l.content_height = ceil_div(count, l.columns) * cell_height + margin;
    }
    return l;
}

void ProgramManager::clamp_scroll()
{
    if (m_current >= m_groups.size())
        return;
    const Layout l = layout();
    Group& group = m_groups[m_current];
    group.scroll = std::clamp(group.scroll, 0, std::max(0, l.content_height - l.programs.height));
}

void ProgramManager::scroll_to_selected()
{
    if (m_current >= m_groups.size() || m_groups[m_current].selected < 0)
        return;
    const Layout l = layout();
    Group& group = m_groups[m_current];
    const int top = margin / 2 + (group.selected / l.columns) * cell_height;
    if (top < group.scroll)
        group.scroll = top;
    else if (top + cell_height > group.scroll + l.programs.height)
        group.scroll = top + cell_height - l.programs.height;
    clamp_scroll();
}

void ProgramManager::open_group(std::size_t index)
{
    if (index >= m_groups.size())
        return;
    m_current = index;
    m_selected_group_icon = -1;
    clamp_scroll();
    render();
}

void ProgramManager::launch_selected()
{
    if (m_current >= m_groups.size())
        return;
    const Group& group = m_groups[m_current];
    if (group.selected >= 0 && group.selected < static_cast<int>(group.programs.size()) && m_launch)
        m_launch(*group.programs[group.selected]);
}

void ProgramManager::render()
{
    if (!m_placed)
        return;
    const Layout l = layout();

    CairoBuffer::show(m_buffer, m_width, m_height, [&](cairo_t* cr) {
        retro::paint_frame(cr, m_width, m_height, window_title, m_active, false, window_buttons, &retro::face);

        if (m_groups.empty()) {
            retro::paint_label(cr, "No applications found", m_width / 2.0, l.client.y + l.client.height / 2.0 - 8,
                l.client.width - 2 * margin, false);
            return;
        }

        // The open group: an inner window holding program icons.
        const Group& group = m_groups[m_current];
        cairo_save(cr);
        cairo_translate(cr, l.group.x, l.group.y);
        retro::paint_frame(cr, l.group.width, l.group.height, group.name, m_active, false, group_buttons,
            &retro::workspace);
        cairo_restore(cr);

        cairo_save(cr);
        cairo_rectangle(cr, l.programs.x, l.programs.y, l.programs.width, l.programs.height);
        cairo_clip(cr);
        const int x0 = l.programs.x + (l.programs.width - l.columns * cell_width) / 2;
        for (std::size_t i = 0; i < group.programs.size(); ++i) {
            const int x = x0 + static_cast<int>(i % l.columns) * cell_width;
            const int y = l.programs.y + margin / 2 + static_cast<int>(i / l.columns) * cell_height - group.scroll;
            if (y + cell_height < l.programs.y || y > l.programs.y + l.programs.height)
                continue;
            const DesktopEntry& entry = *group.programs[i];
            retro::paint_icon(cr, m_icons.get(entry.icon, icon_size), x + (cell_width - icon_size) / 2.0, y + 4,
                icon_size);
            retro::paint_label(cr, entry.name, x + cell_width / 2.0, y + icon_size + 8, cell_width - 4,
                static_cast<int>(i) == group.selected);
        }
        cairo_restore(cr);

        if (l.scrollbar.width > 0) {
            const wlr_box& sb = l.scrollbar;
            retro::fill(cr, sb.x, sb.y, sb.width, sb.height, retro::face);
            retro::paint_arrow_button(cr, sb.x, sb.y, scrollbar_width, true);
            retro::paint_arrow_button(cr, sb.x, sb.y + sb.height - scrollbar_width, scrollbar_width, false);
            const int track = sb.height - 2 * scrollbar_width;
            const int max_scroll = std::max(1, l.content_height - l.programs.height);
            const int thumb = std::max(12, track * l.programs.height / std::max(1, l.content_height));
            const int thumb_y = sb.y + scrollbar_width + (track - thumb) * group.scroll / max_scroll;
            retro::fill(cr, sb.x, thumb_y, scrollbar_width, thumb, retro::face);
            retro::bevel(cr, sb.x, thumb_y, scrollbar_width, thumb, true);
        }

        // The other groups, as icons.
        const auto others = other_groups();
        const int per_row = std::max(1, l.group_icons.width / cell_width);
        for (std::size_t j = 0; j < others.size(); ++j) {
            const int x = l.group_icons.x + static_cast<int>(j % per_row) * cell_width;
            const int y = l.group_icons.y + static_cast<int>(j / per_row) * group_cell_height;
            retro::paint_icon(cr, m_icons.group_icon(icon_size), x + (cell_width - icon_size) / 2.0, y + 2, icon_size);
            retro::paint_label(cr, m_groups[others[j]].name, x + cell_width / 2.0, y + icon_size + 6, cell_width - 4,
                static_cast<int>(others[j]) == m_selected_group_icon);
        }
    });
}

bool ProgramManager::press(double lx, double ly, std::uint32_t time_msec)
{
    if (!m_visible)
        return false;
    if (on_focus)
        on_focus();

    const double x = lx - m_x;
    const double y = ly - m_y;
    const retro::FrameHit hit = retro::hit_test_frame(m_width, m_height, x, y, window_buttons, false);
    switch (hit.part) {
    case retro::FramePart::Control:
        // Double-clicking the control box closes a window; here it minimizes,
        // since the launcher is always there.
        if (m_clicks.click(this, 1, time_msec, lx, ly) && on_minimize)
            on_minimize();
        return false;
    case retro::FramePart::Minimize:
        if (on_minimize)
            on_minimize();
        return false;
    case retro::FramePart::Title:
    case retro::FramePart::Edge:
        m_dragging = true;
        m_drag_x = lx - m_x;
        m_drag_y = ly - m_y;
        return true;
    case retro::FramePart::Client:
        break;
    default:
        return false;
    }

    if (m_groups.empty())
        return false;
    const Layout l = layout();
    Group& group = m_groups[m_current];

    if (l.scrollbar.width > 0 && contains(l.scrollbar, x, y)) {
        if (y < l.scrollbar.y + scrollbar_width)
            group.scroll -= cell_height;
        else if (y >= l.scrollbar.y + l.scrollbar.height - scrollbar_width)
            group.scroll += cell_height;
        else
            group.scroll += (y < l.scrollbar.y + l.scrollbar.height / 2.0) ? -l.programs.height : l.programs.height;
        clamp_scroll();
        render();
        return false;
    }

    if (contains(l.programs, x, y)) {
        const int x0 = l.programs.x + (l.programs.width - l.columns * cell_width) / 2;
        const int col = static_cast<int>(x - x0) / cell_width;
        const int row = static_cast<int>(y - l.programs.y - margin / 2 + group.scroll) / cell_height;
        const int index = row * l.columns + col;
        const bool hit_cell = x >= x0 && col < l.columns && index >= 0 && index < static_cast<int>(group.programs.size());
        group.selected = hit_cell ? index : -1;
        m_selected_group_icon = -1;
        render();
        if (hit_cell && m_clicks.click(group.programs[index], 2, time_msec, lx, ly))
            launch_selected();
        return false;
    }

    if (contains(l.group_icons, x, y)) {
        const auto others = other_groups();
        const int per_row = std::max(1, l.group_icons.width / cell_width);
        const int col = static_cast<int>(x - l.group_icons.x) / cell_width;
        const int row = static_cast<int>(y - l.group_icons.y) / group_cell_height;
        const std::size_t j = static_cast<std::size_t>(row * per_row + col);
        if (col < per_row && j < others.size()) {
            m_selected_group_icon = static_cast<int>(others[j]);
            render();
            if (m_clicks.click(&m_groups[others[j]], 3, time_msec, lx, ly))
                open_group(others[j]);
        }
    }
    return false;
}

void ProgramManager::motion(double lx, double ly)
{
    if (!m_dragging)
        return;
    m_x = static_cast<int>(lx - m_drag_x);
    m_y = static_cast<int>(ly - m_drag_y);
    wlr_scene_node_set_position(&m_tree->node, m_x, m_y);
}

void ProgramManager::release(double lx, double ly)
{
    m_dragging = false;
}

void ProgramManager::scroll(double lx, double ly, double delta)
{
    if (m_groups.empty())
        return;
    // One notch (15 units) scrolls half a row.
    m_groups[m_current].scroll += static_cast<int>(delta / 15.0 * cell_height / 2);
    clamp_scroll();
    render();
}

void ProgramManager::key(xkb_keysym_t sym)
{
    if (m_groups.empty())
        return;
    Group& group = m_groups[m_current];
    const int count = static_cast<int>(group.programs.size());
    const int columns = layout().columns;
    int selected = group.selected;

    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        launch_selected();
        return;
    case XKB_KEY_Tab:
        open_group((m_current + 1) % m_groups.size());
        return;
    case XKB_KEY_ISO_Left_Tab:
        open_group((m_current + m_groups.size() - 1) % m_groups.size());
        return;
    case XKB_KEY_Left: selected = selected < 0 ? 0 : selected - 1; break;
    case XKB_KEY_Right: selected = selected < 0 ? 0 : selected + 1; break;
    case XKB_KEY_Up: selected = selected < 0 ? 0 : selected - columns; break;
    case XKB_KEY_Down: selected = selected < 0 ? 0 : selected + columns; break;
    case XKB_KEY_Home: selected = 0; break;
    case XKB_KEY_End: selected = count - 1; break;
    default: return;
    }
    if (count == 0)
        return;
    group.selected = std::clamp(selected, 0, count - 1);
    scroll_to_selected();
    render();
}

} // namespace wfa
