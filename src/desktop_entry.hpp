#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct wl_event_loop;
struct wl_event_source;

namespace wfa {

// One launchable application from a .desktop file (XDG Desktop Entry spec).
struct DesktopEntry {
    std::string id;          // e.g. "org.gnome.Calculator.desktop"
    std::string name;        // localised
    std::string exec;        // raw Exec value, field codes intact
    std::string icon;        // theme name or absolute path
    std::string path;        // working directory, if any
    std::string startup_wm_class;
    std::vector<std::string> categories;
    bool terminal = false;

    // The launcher group this entry belongs to (from its main category).
    std::string group() const;

    // Exec split into arguments per the spec's quoting rules, with field
    // codes expanded (no files or URLs are passed). Empty on a malformed
    // Exec. Terminal entries are wrapped in `terminal`.
    std::vector<std::string> argv(const std::string& terminal) const;
};

// Name of this desktop for OnlyShowIn/NotShowIn and XDG_CURRENT_DESKTOP.
inline constexpr std::string_view desktop_name = "wayfaraway";

// All applications visible in the XDG data dirs, sorted by name. Earlier
// dirs ($XDG_DATA_HOME first) override later ones with the same id.
std::vector<DesktopEntry> load_desktop_entries();

// The application directories load_desktop_entries() reads.
std::vector<std::filesystem::path> application_dirs();

// Finds the entry for a window's app_id (by desktop id or StartupWMClass).
const DesktopEntry* find_entry_for_app_id(const std::vector<DesktopEntry>& entries, std::string_view app_id);

// Watches the application directories and calls `on_change` (debounced)
// when .desktop files are added, removed or edited.
class DesktopEntryWatcher {
public:
    DesktopEntryWatcher(wl_event_loop* loop, std::function<void()> on_change);
    ~DesktopEntryWatcher();

    DesktopEntryWatcher(const DesktopEntryWatcher&) = delete;
    DesktopEntryWatcher& operator=(const DesktopEntryWatcher&) = delete;

private:
    static int on_inotify(int fd, uint32_t mask, void* data);
    static int on_debounce(void* data);

    std::function<void()> m_on_change;
    int m_fd = -1;
    wl_event_source* m_source = nullptr;
    wl_event_source* m_timer = nullptr;
};

} // namespace wfa
