#pragma once

#include <string>
#include <vector>

#include "wlr.hpp"

typedef struct _cairo_surface cairo_surface_t;

namespace wfa {

// The Alt+Tab overlay: every task as an icon, in most-recently-used order,
// with the selected one's title underneath. It only displays; the Server
// decides when it starts, moves and commits.
class TaskSwitcher {
public:
    struct Task {
        const void* key;       // the View, or the launcher
        std::string title;
        cairo_surface_t* icon; // not owned
        bool minimized = false;
    };

    explicit TaskSwitcher(wlr_scene_tree* parent);
    ~TaskSwitcher();

    TaskSwitcher(const TaskSwitcher&) = delete;
    TaskSwitcher& operator=(const TaskSwitcher&) = delete;

    bool active() const { return m_active; }

    // Shows the overlay centred on `area` with `initial` selected.
    void start(std::vector<Task> tasks, wlr_box area, std::size_t initial);
    // Moves the selection, wrapping around.
    void step(int delta);
    const void* selected() const;
    void hide();

    // The task under a layout point while shown, or nullptr.
    const void* task_at(double lx, double ly) const;

private:
    void render();

    wlr_scene_tree* m_tree;
    wlr_scene_buffer* m_buffer;
    std::vector<Task> m_tasks;
    std::size_t m_selected = 0;
    bool m_active = false;
    wlr_box m_box{};
    int m_columns = 1;
};

} // namespace wfa
