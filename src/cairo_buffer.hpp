#pragma once

#include <cairo.h>
#include <drm_fourcc.h>

#include "wlr.hpp"

namespace wfa {

// A wlr_buffer backed by a cairo image surface, so the compositor can draw
// its own UI with cairo/pango and hand it straight to the scene graph.
//
// Typical use: create(), draw on surface(), then
// wlr_scene_buffer_set_buffer(node, buffer->base()) and drop().
class CairoBuffer {
public:
    static CairoBuffer* create(int width, int height)
    {
        auto* self = new CairoBuffer{};
        self->m_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        wlr_buffer_init(&self->m_base, &impl, width, height);
        return self;
    }

    cairo_surface_t* surface() const { return m_surface; }
    wlr_buffer* base() { return &m_base; }

    // Releases our reference; the scene keeps the buffer alive while shown.
    void drop()
    {
        cairo_surface_flush(m_surface);
        wlr_buffer_drop(&m_base);
    }

    // Draws into a new buffer and shows it on `node` (replacing what was there).
    template <typename Paint>
    static void show(wlr_scene_buffer* node, int width, int height, Paint&& paint)
    {
        if (width <= 0 || height <= 0) {
            wlr_scene_buffer_set_buffer(node, nullptr);
            return;
        }
        CairoBuffer* buffer = create(width, height);
        cairo_t* cr = cairo_create(buffer->m_surface);
        paint(cr);
        cairo_destroy(cr);
        cairo_surface_flush(buffer->m_surface);
        wlr_scene_buffer_set_buffer(node, buffer->base());
        buffer->drop();
    }

private:
    CairoBuffer() = default;

    static CairoBuffer* from(wlr_buffer* buffer) { return reinterpret_cast<CairoBuffer*>(buffer); }

    static void destroy(wlr_buffer* buffer)
    {
        CairoBuffer* self = from(buffer);
        wlr_buffer_finish(buffer);
        cairo_surface_destroy(self->m_surface);
        delete self;
    }

    static bool begin_access(wlr_buffer* buffer, uint32_t flags, void** data, uint32_t* format, size_t* stride)
    {
        if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
            return false;
        CairoBuffer* self = from(buffer);
        *data = cairo_image_surface_get_data(self->m_surface);
        *format = DRM_FORMAT_ARGB8888;
        *stride = static_cast<size_t>(cairo_image_surface_get_stride(self->m_surface));
        return true;
    }

    static void end_access(wlr_buffer*) { }

    static constexpr wlr_buffer_impl impl = {
        .destroy = &CairoBuffer::destroy,
        .get_dmabuf = nullptr,
        .get_shm = nullptr,
        .begin_data_ptr_access = &CairoBuffer::begin_access,
        .end_data_ptr_access = &CairoBuffer::end_access,
    };

    wlr_buffer m_base{}; // must stay first
    cairo_surface_t* m_surface = nullptr;
};

} // namespace wfa
