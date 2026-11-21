#include "icon_loader.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <vector>

#include <librsvg/rsvg.h>

#include "retro_theme.hpp"

namespace wfa {

namespace {

namespace fs = std::filesystem;

std::vector<fs::path> icon_base_dirs()
{
    std::vector<fs::path> dirs;
    if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home && *data_home)
        dirs.emplace_back(fs::path(data_home) / "icons");
    if (const char* home = std::getenv("HOME"); home && *home) {
        dirs.emplace_back(fs::path(home) / ".local/share/icons");
        dirs.emplace_back(fs::path(home) / ".icons");
    }
    const char* data_dirs = std::getenv("XDG_DATA_DIRS");
    std::string_view list = (data_dirs && *data_dirs) ? data_dirs : "/usr/local/share:/usr/share";
    for (auto dir : std::views::split(list, ':')) {
        std::string d(dir.begin(), dir.end());
        if (!d.empty())
            dirs.emplace_back(fs::path(d) / "icons");
    }
    return dirs;
}

// Simplified icon theme lookup: hicolor (where applications install their
// icons) first, then common full themes for the rest.
constexpr std::array themes = { "hicolor", "Adwaita", "breeze", "Papirus", "elementary", "gnome" };
constexpr std::array contexts = { "apps", "legacy" };

// Size directories to try for a requested size: exact, scalable, then the
// nearest larger PNGs (downscaling looks better), then smaller ones.
std::vector<std::string> size_dirs(int size)
{
    std::vector<std::string> dirs;
    dirs.push_back(std::format("{0}x{0}", size));
    dirs.emplace_back("scalable");
    for (int s : { 16, 22, 24, 32, 48, 64, 96, 128, 256, 512 }) {
        if (s > size)
            dirs.push_back(std::format("{0}x{0}", s));
    }
    for (int s : { 512, 256, 128, 96, 64, 48, 32, 24, 22, 16 }) {
        if (s < size)
            dirs.push_back(std::format("{0}x{0}", s));
    }
    return dirs;
}

std::optional<fs::path> find_icon_file(const std::string& name, int size)
{
    std::error_code ec;
    const auto exists = [&](const fs::path& p) { return fs::is_regular_file(p, ec); };

    if (name.starts_with('/'))
        return exists(name) ? std::optional<fs::path>(name) : std::nullopt;

    const auto bases = icon_base_dirs();
    const auto sizes = size_dirs(size);
    for (const char* theme : themes) {
        for (const auto& base : bases) {
            const fs::path theme_dir = base / theme;
            if (!fs::is_directory(theme_dir, ec))
                continue;
            for (const auto& sz : sizes) {
                for (const char* context : contexts) {
                    // Both the common "48x48/apps" and breeze's "apps/48" layouts.
                    const std::string number = sz.substr(0, sz.find('x'));
                    for (const fs::path& dir : { theme_dir / sz / context, theme_dir / context / number }) {
                        for (const char* ext : { ".svg", ".png" }) {
                            fs::path candidate = dir / (name + ext);
                            if (exists(candidate))
                                return candidate;
                        }
                    }
                }
            }
        }
    }

    for (const char* pixmaps : { "/usr/share/pixmaps", "/usr/local/share/pixmaps" }) {
        for (const char* ext : { ".png", ".svg" }) {
            fs::path candidate = fs::path(pixmaps) / (name + ext);
            if (exists(candidate))
                return candidate;
        }
    }
    return std::nullopt;
}

cairo_surface_t* new_surface(int size)
{
    return cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
}

cairo_surface_t* load_svg(const fs::path& file, int size)
{
    GError* error = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_file(file.c_str(), &error);
    if (!handle) {
        if (error)
            g_error_free(error);
        return nullptr;
    }
    cairo_surface_t* surface = new_surface(size);
    cairo_t* cr = cairo_create(surface);
    const RsvgRectangle viewport{ 0, 0, static_cast<double>(size), static_cast<double>(size) };
    const bool ok = rsvg_handle_render_document(handle, cr, &viewport, &error);
    cairo_destroy(cr);
    g_object_unref(handle);
    if (error)
        g_error_free(error);
    if (!ok) {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    return surface;
}

cairo_surface_t* load_png(const fs::path& file, int size)
{
    cairo_surface_t* image = cairo_image_surface_create_from_png(file.c_str());
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(image);
        return nullptr;
    }
    cairo_surface_t* surface = new_surface(size);
    cairo_t* cr = cairo_create(surface);
    retro::paint_icon(cr, image, 0, 0, size);
    cairo_destroy(cr);
    cairo_surface_destroy(image);
    return surface;
}

} // namespace

IconLoader::~IconLoader()
{
    clear();
}

void IconLoader::clear()
{
    for (auto& [key, surface] : m_cache)
        cairo_surface_destroy(surface);
    m_cache.clear();
}

cairo_surface_t* IconLoader::get(const std::string& name, int size)
{
    const auto key = std::make_pair(name, size);
    if (auto it = m_cache.find(key); it != m_cache.end())
        return it->second;

    cairo_surface_t* surface = name.empty() ? nullptr : load(name, size);
    if (!surface)
        surface = fallback(size);
    m_cache.emplace(key, surface);
    return surface;
}

cairo_surface_t* IconLoader::load(const std::string& name, int size)
{
    const auto file = find_icon_file(name, size);
    if (!file)
        return nullptr;
    if (file->extension() == ".svg")
        return load_svg(*file, size);
    return load_png(*file, size);
}

// A plain program icon: a little window with a title bar.
cairo_surface_t* IconLoader::fallback(int size)
{
    cairo_surface_t* surface = new_surface(size);
    cairo_t* cr = cairo_create(surface);
    const double m = size / 8.0;
    const double w = size - 2 * m;
    retro::fill(cr, m, m, w, w, retro::white);
    retro::fill(cr, m, m, w, size / 5.0, retro::active_title);
    retro::set_color(cr, retro::black);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, m + 0.5, m + 0.5, w - 1, w - 1);
    cairo_stroke(cr);
    cairo_destroy(cr);
    return surface;
}

// A program group: a window holding a grid of tiny program icons.
cairo_surface_t* IconLoader::group_icon(int size)
{
    const auto key = std::make_pair(std::string("#group"), size);
    if (auto it = m_cache.find(key); it != m_cache.end())
        return it->second;

    cairo_surface_t* surface = new_surface(size);
    cairo_t* cr = cairo_create(surface);
    const double m = size / 10.0;
    const double w = size - 2 * m;
    const double title = size / 6.0;
    retro::fill(cr, m, m, w, w, retro::white);
    retro::fill(cr, m, m, w, title, retro::active_title);
    const double cell = (w - 4) / 3.0;
    const retro::Color colors[] = { { 0.8, 0, 0 }, { 0, 0.6, 0 }, { 0.8, 0.7, 0 }, { 0, 0.5, 0.5 } };
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            retro::fill(cr, m + 2 + col * cell + cell * 0.2, m + title + 3 + row * cell + cell * 0.15, cell * 0.6,
                cell * 0.6, colors[(row * 3 + col) % 4]);
        }
    }
    retro::set_color(cr, retro::black);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, m + 0.5, m + 0.5, w - 1, w - 1);
    cairo_stroke(cr);
    cairo_destroy(cr);

    m_cache.emplace(key, surface);
    return surface;
}

} // namespace wfa
