#pragma once

#include <map>
#include <string>
#include <utility>

#include <cairo.h>

namespace wfa {

// Finds and loads application icons (freedesktop icon theme layout) as
// cairo surfaces of a requested size. PNG and SVG are supported. Results,
// including misses, are cached for the lifetime of the loader.
class IconLoader {
public:
    IconLoader() = default;
    ~IconLoader();

    IconLoader(const IconLoader&) = delete;
    IconLoader& operator=(const IconLoader&) = delete;

    // `name` is a theme icon name or an absolute path. Never returns null:
    // unknown icons get a generic program icon. Owned by the loader.
    cairo_surface_t* get(const std::string& name, int size);

    // A generic icon for program groups.
    cairo_surface_t* group_icon(int size);

    // Drops the cache, e.g. after applications were installed.
    void clear();

private:
    cairo_surface_t* load(const std::string& name, int size);
    cairo_surface_t* fallback(int size);

    std::map<std::pair<std::string, int>, cairo_surface_t*> m_cache;
};

} // namespace wfa
