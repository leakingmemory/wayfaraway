#include "task_switcher.hpp"

#include <algorithm>

#include "cairo_buffer.hpp"
#include "retro_theme.hpp"

namespace wfa {

namespace {

constexpr int max_columns = 10;
constexpr int cell = 56;
constexpr int icon_size = 32;
constexpr int padding = 12;
constexpr int title_height = 34;

} // namespace

TaskSwitcher::TaskSwitcher(wlr_scene_tree* parent)
    : m_tree(wlr_scene_tree_create(parent))
    , m_buffer(wlr_scene_buffer_create(m_tree, nullptr))
{
    wlr_scene_node_set_enabled(&m_tree->node, false);
}

TaskSwitcher::~TaskSwitcher()
{
    wlr_scene_node_destroy(&m_tree->node);
}

void TaskSwitcher::start(std::vector<Task> tasks, wlr_box area, std::size_t initial)
{
    m_tasks = std::move(tasks);
    if (m_tasks.empty())
        return;
    m_selected = std::min(initial, m_tasks.size() - 1);
    m_active = true;

    m_columns = std::min(static_cast<int>(m_tasks.size()), max_columns);
    const int rows = (static_cast<int>(m_tasks.size()) + m_columns - 1) / m_columns;
    m_box.width = m_columns * cell + 2 * padding;
    m_box.height = rows * cell + 2 * padding + title_height;
    m_box.x = area.x + (area.width - m_box.width) / 2;
    m_box.y = area.y + (area.height - m_box.height) / 2;

    wlr_scene_node_set_position(&m_tree->node, m_box.x, m_box.y);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    render();
}

void TaskSwitcher::step(int delta)
{
    if (!m_active || m_tasks.empty())
        return;
    const int n = static_cast<int>(m_tasks.size());
    m_selected = static_cast<std::size_t>(((static_cast<int>(m_selected) + delta) % n + n) % n);
    render();
}

const void* TaskSwitcher::selected() const
{
    return m_active && m_selected < m_tasks.size() ? m_tasks[m_selected].key : nullptr;
}

void TaskSwitcher::hide()
{
    m_active = false;
    m_tasks.clear();
    wlr_scene_node_set_enabled(&m_tree->node, false);
}

const void* TaskSwitcher::task_at(double lx, double ly) const
{
    if (!m_active)
        return nullptr;
    const double x = lx - m_box.x - padding;
    const double y = ly - m_box.y - padding;
    if (x < 0 || y < 0)
        return nullptr;
    const int col = static_cast<int>(x) / cell;
    const int row = static_cast<int>(y) / cell;
    const std::size_t index = static_cast<std::size_t>(row * m_columns + col);
    if (col >= m_columns || index >= m_tasks.size())
        return nullptr;
    return m_tasks[index].key;
}

void TaskSwitcher::render()
{
    CairoBuffer::show(m_buffer, m_box.width, m_box.height, [&](cairo_t* cr) {
        // A raised grey panel with a black outline.
        retro::fill(cr, 0, 0, m_box.width, m_box.height, retro::face);
        retro::bevel(cr, 1, 1, m_box.width - 2, m_box.height - 2, true);
        retro::set_color(cr, retro::black);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, 0.5, 0.5, m_box.width - 1, m_box.height - 1);
        cairo_stroke(cr);

        for (std::size_t i = 0; i < m_tasks.size(); ++i) {
            const Task& task = m_tasks[i];
            const double x = padding + static_cast<double>(i % m_columns) * cell;
            const double y = padding + static_cast<double>(i / m_columns) * cell;

            if (i == m_selected) {
                // Sunken, like a pressed button.
                retro::fill(cr, x + 2, y + 2, cell - 4, cell - 4, retro::white);
                retro::bevel(cr, x + 2, y + 2, cell - 4, cell - 4, false);
                retro::set_color(cr, retro::selection);
                cairo_set_line_width(cr, 2);
                cairo_rectangle(cr, x + 4, y + 4, cell - 8, cell - 8);
                cairo_stroke(cr);
            }

            const double ix = x + (cell - icon_size) / 2.0;
            const double iy = y + (cell - icon_size) / 2.0;
            if (task.minimized) {
                // Faded: it is restored when chosen.
                cairo_push_group(cr);
                retro::paint_icon(cr, task.icon, ix, iy, icon_size);
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, 0.45);
            } else {
                retro::paint_icon(cr, task.icon, ix, iy, icon_size);
            }
        }

        const Task& current = m_tasks[m_selected];
        const std::string title = current.minimized ? current.title + " (minimized)" : current.title;
        const int rows = (static_cast<int>(m_tasks.size()) + m_columns - 1) / m_columns;
        retro::paint_label(cr, title, m_box.width / 2.0, padding + rows * cell + 6, m_box.width - 2 * padding, false);
    });
}

} // namespace wfa
