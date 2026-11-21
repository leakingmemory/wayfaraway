#include "desktop_entry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <system_error>
#include <unordered_map>

#include <sys/inotify.h>
#include <unistd.h>

#include "wlr.hpp"

namespace wfa {

namespace {

namespace fs = std::filesystem;

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

// The general escapes of the spec: \s \n \t \r \\.
std::string unescape(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out.push_back(value[i]);
            continue;
        }
        switch (value[++i]) {
        case 's': out.push_back(' '); break;
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case '\\': out.push_back('\\'); break;
        default:
            // Kept for list values (\;) and Exec quoting, handled later.
            out.push_back('\\');
            out.push_back(value[i]);
            break;
        }
    }
    return out;
}

std::vector<std::string> split_list(std::string_view value)
{
    std::vector<std::string> items;
    std::string current;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == ';') {
            current.push_back(';');
            ++i;
        } else if (value[i] == ';') {
            if (!current.empty())
                items.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(value[i]);
        }
    }
    if (!current.empty())
        items.push_back(std::move(current));
    return items;
}

// Locale suffixes to try for localised keys, most specific first:
// lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang.
std::vector<std::string> locale_candidates()
{
    const char* names[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    std::string locale;
    for (const char* name : names) {
        if (const char* value = std::getenv(name); value && *value) {
            locale = value;
            break;
        }
    }
    if (locale.empty() || locale == "C" || locale == "POSIX")
        return {};

    std::string modifier;
    if (auto at = locale.find('@'); at != std::string::npos) {
        modifier = locale.substr(at);
        locale.erase(at);
    }
    if (auto dot = locale.find('.'); dot != std::string::npos)
        locale.erase(dot);
    std::string lang = locale;
    std::string country;
    if (auto underscore = locale.find('_'); underscore != std::string::npos) {
        lang = locale.substr(0, underscore);
        country = locale.substr(underscore);
    }

    std::vector<std::string> candidates;
    if (!country.empty() && !modifier.empty())
        candidates.push_back(lang + country + modifier);
    if (!country.empty())
        candidates.push_back(lang + country);
    if (!modifier.empty())
        candidates.push_back(lang + modifier);
    candidates.push_back(lang);
    return candidates;
}

using Keys = std::unordered_map<std::string, std::string>;

std::optional<Keys> parse_file(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        return std::nullopt;

    Keys keys;
    bool in_entry = false;
    std::string line;
    while (std::getline(in, line)) {
        std::string_view l = trim(line);
        if (l.empty() || l.front() == '#')
            continue;
        if (l.front() == '[') {
            in_entry = l == "[Desktop Entry]";
            continue;
        }
        if (!in_entry)
            continue;
        auto eq = l.find('=');
        if (eq == std::string_view::npos)
            continue;
        keys.try_emplace(std::string(trim(l.substr(0, eq))), std::string(trim(l.substr(eq + 1))));
    }
    return keys;
}

std::string get(const Keys& keys, const std::string& key)
{
    auto it = keys.find(key);
    return it == keys.end() ? std::string{} : unescape(it->second);
}

std::string get_localised(const Keys& keys, const std::string& key, const std::vector<std::string>& locales)
{
    for (const auto& locale : locales) {
        auto it = keys.find(key + "[" + locale + "]");
        if (it != keys.end())
            return unescape(it->second);
    }
    return get(keys, key);
}

bool get_bool(const Keys& keys, const std::string& key)
{
    return get(keys, key) == "true";
}

bool contains_desktop(const std::vector<std::string>& desktops)
{
    return std::ranges::any_of(desktops, [](const std::string& d) { return iequals(d, desktop_name); });
}

bool executable_exists(const std::string& program)
{
    if (program.empty())
        return false;
    if (program.find('/') != std::string::npos)
        return access(program.c_str(), X_OK) == 0;
    const char* path = std::getenv("PATH");
    std::string_view dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    for (auto dir : std::views::split(dirs, ':')) {
        std::string candidate(dir.begin(), dir.end());
        if (candidate.empty())
            continue;
        candidate += "/" + program;
        if (access(candidate.c_str(), X_OK) == 0)
            return true;
    }
    return false;
}

// Exec quoting: arguments are separated by spaces; double quotes group, and
// inside them \" \` \$ \\ are escapes. Returns nullopt if malformed.
std::optional<std::vector<std::string>> split_exec(std::string_view exec)
{
    std::vector<std::string> args;
    std::string current;
    bool in_quotes = false;
    bool have_token = false;
    for (std::size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (in_quotes) {
            if (c == '\\' && i + 1 < exec.size() && std::strchr("\"`$\\", exec[i + 1]))
                current.push_back(exec[++i]);
            else if (c == '"')
                in_quotes = false;
            else
                current.push_back(c);
        } else if (c == ' ' || c == '\t') {
            if (have_token)
                args.push_back(std::move(current));
            current.clear();
            have_token = false;
        } else if (c == '"') {
            in_quotes = true;
            have_token = true;
        } else {
            current.push_back(c);
            have_token = true;
        }
    }
    if (in_quotes)
        return std::nullopt;
    if (have_token)
        args.push_back(std::move(current));
    return args;
}

// How a terminal emulator wants the command it should run.
std::vector<std::string> terminal_prefix(const std::string& terminal)
{
    auto words = split_exec(terminal).value_or(std::vector<std::string>{ terminal });
    if (words.empty())
        return {};
    const std::string base = fs::path(words.front()).filename().string();
    if (base == "foot" || base == "kitty") {
        // Take the command directly.
    } else if (base == "gnome-terminal" || base == "kgx" || base == "ptyxis") {
        words.emplace_back("--");
    } else if (base == "wezterm") {
        words.emplace_back("start");
        words.emplace_back("--");
    } else {
        words.emplace_back("-e");
    }
    return words;
}

} // namespace

std::string DesktopEntry::group() const
{
    static const std::array<std::pair<std::string_view, std::string_view>, 13> main_categories = { {
        { "AudioVideo", "Multimedia" },
        { "Audio", "Multimedia" },
        { "Video", "Multimedia" },
        { "Development", "Development" },
        { "Education", "Education" },
        { "Game", "Games" },
        { "Graphics", "Graphics" },
        { "Network", "Internet" },
        { "Office", "Office" },
        { "Science", "Science" },
        { "Settings", "Settings" },
        { "System", "System" },
        { "Utility", "Accessories" },
    } };
    for (const auto& category : categories) {
        for (const auto& [main, group] : main_categories) {
            if (category == main)
                return std::string(group);
        }
    }
    return "Other";
}

std::vector<std::string> DesktopEntry::argv(const std::string& terminal) const
{
    auto words = split_exec(exec);
    if (!words || words->empty())
        return {};

    std::vector<std::string> args;
    if (this->terminal)
        args = terminal_prefix(terminal);

    for (const std::string& word : *words) {
        // Codes for files and URLs: we launch without any.
        static const std::set<std::string_view> dropped = { "%f", "%F", "%u", "%U", "%d", "%D", "%n", "%N", "%v", "%m" };
        if (dropped.contains(word))
            continue;
        if (word == "%i") {
            if (!icon.empty()) {
                args.emplace_back("--icon");
                args.push_back(icon);
            }
            continue;
        }

        std::string arg;
        for (std::size_t i = 0; i < word.size(); ++i) {
            if (word[i] != '%' || i + 1 >= word.size()) {
                arg.push_back(word[i]);
                continue;
            }
            switch (word[++i]) {
            case '%': arg.push_back('%'); break;
            case 'c': arg += name; break;
            default: break; // other codes inside a word expand to nothing
            }
        }
        args.push_back(std::move(arg));
    }
    return args;
}

std::vector<fs::path> application_dirs()
{
    std::vector<fs::path> dirs;
    if (const char* home = std::getenv("XDG_DATA_HOME"); home && *home)
        dirs.emplace_back(fs::path(home) / "applications");
    else if (const char* user_home = std::getenv("HOME"); user_home && *user_home)
        dirs.emplace_back(fs::path(user_home) / ".local/share/applications");

    const char* data_dirs = std::getenv("XDG_DATA_DIRS");
    std::string_view list = (data_dirs && *data_dirs) ? data_dirs : "/usr/local/share:/usr/share";
    for (auto dir : std::views::split(list, ':')) {
        std::string d(dir.begin(), dir.end());
        if (!d.empty())
            dirs.emplace_back(fs::path(d) / "applications");
    }
    return dirs;
}

std::vector<DesktopEntry> load_desktop_entries()
{
    const auto locales = locale_candidates();
    std::set<std::string> seen; // desktop ids, including hidden ones
    std::vector<DesktopEntry> entries;

    for (const fs::path& dir : application_dirs()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec)
                break;
            const fs::path& file = it->path();
            if (file.extension() != ".desktop" || !it->is_regular_file(ec))
                continue;

            // The desktop id: the path below applications/, with / as -.
            std::string id = fs::relative(file, dir, ec).string();
            std::ranges::replace(id, '/', '-');
            // The first directory with an id wins, even if it hides it.
            if (!seen.insert(id).second)
                continue;

            auto keys = parse_file(file);
            if (!keys || get(*keys, "Type") != "Application")
                continue;
            if (get_bool(*keys, "NoDisplay") || get_bool(*keys, "Hidden"))
                continue;
            const auto only = split_list(get(*keys, "OnlyShowIn"));
            if (!only.empty() && !contains_desktop(only))
                continue;
            if (contains_desktop(split_list(get(*keys, "NotShowIn"))))
                continue;
            if (const std::string try_exec = get(*keys, "TryExec"); !try_exec.empty() && !executable_exists(try_exec))
                continue;

            DesktopEntry entry;
            entry.id = std::move(id);
            entry.name = get_localised(*keys, "Name", locales);
            entry.exec = get(*keys, "Exec");
            entry.icon = get_localised(*keys, "Icon", locales);
            entry.path = get(*keys, "Path");
            entry.startup_wm_class = get(*keys, "StartupWMClass");
            entry.categories = split_list(get(*keys, "Categories"));
            entry.terminal = get_bool(*keys, "Terminal");
            if (entry.name.empty() || entry.exec.empty())
                continue;
            entries.push_back(std::move(entry));
        }
    }

    std::ranges::sort(entries, [](const DesktopEntry& a, const DesktopEntry& b) {
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
            [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
    });
    wlr_log(WLR_INFO, "Launcher: %zu applications", entries.size());
    return entries;
}

const DesktopEntry* find_entry_for_app_id(const std::vector<DesktopEntry>& entries, std::string_view app_id)
{
    if (app_id.empty())
        return nullptr;
    const std::string with_suffix = std::string(app_id) + ".desktop";
    for (const auto& e : entries) {
        if (e.id == with_suffix)
            return &e;
    }
    for (const auto& e : entries) {
        if (iequals(e.id, with_suffix) || iequals(e.startup_wm_class, app_id))
            return &e;
    }
    // Reverse-DNS ids: "org.gnome.Calculator.desktop" for app_id "calculator".
    for (const auto& e : entries) {
        std::string_view id = e.id;
        id.remove_suffix(std::min(id.size(), std::string_view(".desktop").size()));
        if (auto dot = id.rfind('.'); dot != std::string_view::npos && iequals(id.substr(dot + 1), app_id))
            return &e;
    }
    return nullptr;
}

DesktopEntryWatcher::DesktopEntryWatcher(wl_event_loop* loop, std::function<void()> on_change)
    : m_on_change(std::move(on_change))
{
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd < 0) {
        wlr_log_errno(WLR_ERROR, "Launcher: inotify unavailable; new applications need a restart");
        return;
    }
    constexpr uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;
    for (const auto& dir : application_dirs()) {
        std::error_code ec;
        if (fs::is_directory(dir, ec))
            inotify_add_watch(m_fd, dir.c_str(), mask);
    }
    m_source = wl_event_loop_add_fd(loop, m_fd, WL_EVENT_READABLE, &DesktopEntryWatcher::on_inotify, this);
    m_timer = wl_event_loop_add_timer(loop, &DesktopEntryWatcher::on_debounce, this);
}

DesktopEntryWatcher::~DesktopEntryWatcher()
{
    if (m_timer)
        wl_event_source_remove(m_timer);
    if (m_source)
        wl_event_source_remove(m_source);
    if (m_fd >= 0)
        close(m_fd);
}

int DesktopEntryWatcher::on_inotify(int fd, uint32_t mask, void* data)
{
    auto* self = static_cast<DesktopEntryWatcher*>(data);
    alignas(inotify_event) char buffer[4096];
    while (read(fd, buffer, sizeof(buffer)) > 0) {
    }
    // Package managers touch many files at once; reload once they settle.
    if (self->m_timer)
        wl_event_source_timer_update(self->m_timer, 500);
    return 0;
}

int DesktopEntryWatcher::on_debounce(void* data)
{
    auto* self = static_cast<DesktopEntryWatcher*>(data);
    if (self->m_on_change)
        self->m_on_change();
    return 0;
}

} // namespace wfa
