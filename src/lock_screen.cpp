#include "lock_screen.hpp"

#include <chrono>
#include <cstring>
#include <ctime>
#include <format>

#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>

namespace wfa {

namespace {

// A wlr_buffer backed by a cairo image surface, so the compositor can draw
// UI with cairo/pango and hand it straight to the scene graph.
struct CairoBuffer {
    wlr_buffer base; // must stay first
    cairo_surface_t* surface;

    static CairoBuffer* from(wlr_buffer* buffer) { return reinterpret_cast<CairoBuffer*>(buffer); }

    static void destroy(wlr_buffer* buffer)
    {
        auto* self = from(buffer);
        wlr_buffer_finish(buffer);
        cairo_surface_destroy(self->surface);
        delete self;
    }

    static bool begin_access(wlr_buffer* buffer, uint32_t flags, void** data, uint32_t* format, size_t* stride)
    {
        if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
            return false;
        auto* self = from(buffer);
        *data = cairo_image_surface_get_data(self->surface);
        *format = DRM_FORMAT_ARGB8888;
        *stride = static_cast<size_t>(cairo_image_surface_get_stride(self->surface));
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

    static CairoBuffer* create(int width, int height)
    {
        auto* self = new CairoBuffer{};
        self->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        wlr_buffer_init(&self->base, &impl, width, height);
        return self;
    }
};

struct Rgba {
    double r, g, b, a = 1.0;
};

void set_color(cairo_t* cr, Rgba c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

// Draws `text` horizontally centred on `cx` with its top edge at `y`.
// Returns the height of the laid-out text.
int draw_text(cairo_t* cr, std::string_view text, const char* font, double cx, double y, Rgba color)
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));

    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    set_color(cr, color);
    cairo_move_to(cr, cx - w / 2.0, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    return h;
}

void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

std::string clock_text()
{
    auto now = std::chrono::zoned_time{ std::chrono::current_zone(), std::chrono::system_clock::now() };
    return std::format("{:%H:%M}", now);
}

std::string date_text()
{
    auto now = std::chrono::zoned_time{ std::chrono::current_zone(), std::chrono::system_clock::now() };
    return std::format("{:%A, %e %B}", now);
}

constexpr std::size_t max_password_length = 512;

} // namespace

LockScreen::LockScreen(wl_event_loop* loop, wlr_scene_tree* parent, Authenticator& authenticator)
    : m_loop(loop)
    , m_authenticator(authenticator)
    , m_tree(wlr_scene_tree_create(parent))
    , m_buffer_node(wlr_scene_buffer_create(m_tree, nullptr))
    , m_timer(wl_event_loop_add_timer(loop, &LockScreen::on_tick, this))
{
    wl_event_source_timer_update(m_timer, 1000);
}

LockScreen::~LockScreen()
{
    clear_password();
    wl_event_source_remove(m_timer);
    wlr_scene_node_destroy(&m_tree->node);
}

void LockScreen::lock()
{
    ++m_generation;
    clear_password();
    m_status.clear();
    m_status_is_error = false;
    if (!m_locked) {
        m_locked = true;
        wl_event_source_timer_update(m_timer, 1000);
    }
    wlr_scene_node_set_enabled(&m_tree->node, true);
    render();
}

void LockScreen::set_layout(int width, int height, wlr_box primary)
{
    if (width == m_width && height == m_height && wlr_box_equal(&primary, &m_primary))
        return;
    m_width = width;
    m_height = height;
    m_primary = primary;
    render();
}

void LockScreen::handle_key(xkb_keysym_t sym, std::string_view utf8)
{
    if (!m_locked || m_authenticator.busy())
        return;

    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        submit();
        return;
    case XKB_KEY_Escape:
        clear_password();
        break;
    case XKB_KEY_BackSpace:
        // Drop one UTF-8 code point.
        while (!m_password.empty()) {
            bool continuation = (static_cast<unsigned char>(m_password.back()) & 0xC0) == 0x80;
            m_password.pop_back();
            if (!continuation)
                break;
        }
        break;
    default:
        if (utf8.empty() || static_cast<unsigned char>(utf8.front()) < 0x20)
            return;
        if (m_password.size() + utf8.size() > max_password_length)
            return;
        m_password.append(utf8);
        break;
    }

    if (m_status_is_error) {
        m_status.clear();
        m_status_is_error = false;
    }
    render();
}

void LockScreen::set_caps_lock(bool on)
{
    if (on == m_caps_lock)
        return;
    m_caps_lock = on;
    if (m_locked)
        render();
}

void LockScreen::submit()
{
    if (m_password.empty())
        return;

    m_status = "Verifying…";
    m_status_is_error = false;
    render();

    std::string password = std::move(m_password);
    clear_password();

    m_authenticator.authenticate(std::move(password), [this, generation = m_generation](bool success, std::string message) {
        if (!m_locked || generation != m_generation)
            return;
        if (success) {
            m_locked = false;
            m_status.clear();
            wl_event_source_timer_update(m_timer, 0);
            wlr_scene_node_set_enabled(&m_tree->node, false);
            wlr_log(WLR_INFO, "Unlocked by %s", m_authenticator.user().c_str());
            if (on_unlock)
                on_unlock();
            return;
        }
        wlr_log(WLR_INFO, "Unlock attempt failed: %s", message.c_str());
        m_status = "Incorrect password";
        m_status_is_error = true;
        render();
    });
}

void LockScreen::clear_password()
{
    explicit_bzero(m_password.data(), m_password.size());
    m_password.clear();
}

int LockScreen::on_tick(void* data)
{
    auto* self = static_cast<LockScreen*>(data);
    if (!self->m_locked)
        return 0;
    if (clock_text() != self->m_rendered_clock)
        self->render();
    wl_event_source_timer_update(self->m_timer, 1000);
    return 0;
}

void LockScreen::render()
{
    if (m_width <= 0 || m_height <= 0)
        return;

    auto* buffer = CairoBuffer::create(m_width, m_height);
    cairo_t* cr = cairo_create(buffer->surface);

    const double w = m_width;
    const double h = m_height;
    const double cx = m_primary.x + m_primary.width / 2.0;

    cairo_pattern_t* bg = cairo_pattern_create_linear(0, 0, w, h);
    cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.07, 0.09, 0.14);
    cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.13, 0.10, 0.20);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    const Rgba text{ 0.93, 0.94, 0.97 };
    const Rgba muted{ 0.65, 0.68, 0.78 };

    m_rendered_clock = clock_text();
    double y = m_primary.y + m_primary.height * 0.28;
    y += draw_text(cr, m_rendered_clock, "Sans Light 96", cx, y, text);
    y += draw_text(cr, date_text(), "Sans 20", cx, y, muted);

    y += 56;
    y += draw_text(cr, m_authenticator.user(), "Sans Bold 18", cx, y, text) + 16;

    // Password field.
    const double field_w = 360, field_h = 48;
    const double fx = cx - field_w / 2;
    rounded_rect(cr, fx, y, field_w, field_h, 12);
    set_color(cr, { 1, 1, 1, 0.08 });
    cairo_fill_preserve(cr);
    set_color(cr, m_status_is_error ? Rgba{ 0.90, 0.35, 0.40, 0.9 } : Rgba{ 1, 1, 1, 0.25 });
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    std::size_t glyphs = 0;
    for (unsigned char c : m_password)
        glyphs += (c & 0xC0) != 0x80;

    if (glyphs == 0) {
        draw_text(cr, "Password", "Sans 14", cx, y + 13, { 1, 1, 1, 0.35 });
    } else {
        const double dot_spacing = 18;
        const std::size_t shown = std::min<std::size_t>(glyphs, static_cast<std::size_t>((field_w - 32) / dot_spacing));
        double dx = cx - (shown - 1) * dot_spacing / 2.0;
        set_color(cr, text);
        for (std::size_t i = 0; i < shown; ++i, dx += dot_spacing) {
            cairo_arc(cr, dx, y + field_h / 2, 5, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    }
    y += field_h + 18;

    if (!m_status.empty())
        y += draw_text(cr, m_status, "Sans 13", cx, y, m_status_is_error ? Rgba{ 0.95, 0.45, 0.50 } : muted);
    else
        y += draw_text(cr, "Type your password and press Enter", "Sans 12", cx, y, { 1, 1, 1, 0.35 });

    if (m_caps_lock)
        draw_text(cr, "Caps Lock is on", "Sans Bold 12", cx, y + 10, { 0.96, 0.76, 0.35 });

    cairo_destroy(cr);
    cairo_surface_flush(buffer->surface);

    wlr_scene_buffer_set_buffer(m_buffer_node, &buffer->base);
    wlr_buffer_drop(&buffer->base);
}

} // namespace wfa
