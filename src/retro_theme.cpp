#include "retro_theme.hpp"

#include <algorithm>
#include <string>

#include <pango/pangocairo.h>

#include "wlr.hpp"

namespace wfa::retro {

namespace {

// Title-bar buttons are square, one pixel short of the bar (which ends in a
// black line), and separated from the title by a black line.
constexpr int button_size = title_height - 1;

struct TitleLayout {
    double control_x = -1;  // left of the control box, or -1
    double minimize_x = -1;
    double maximize_x = -1;
    double text_left = 0;
    double text_right = 0;
};

TitleLayout title_layout(int w, FrameButtons buttons)
{
    TitleLayout l;
    double left = border;
    double right = w - border;
    if (buttons.control) {
        l.control_x = left;
        left += button_size + 1;
    }
    if (buttons.maximize) {
        right -= button_size;
        l.maximize_x = right;
        right -= 1;
    }
    if (buttons.minimize) {
        right -= button_size;
        l.minimize_x = right;
        right -= 1;
    }
    l.text_left = left;
    l.text_right = right;
    return l;
}

void line(cairo_t* cr, double x0, double y0, double x1, double y1, Color color)
{
    set_color(cr, color);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    cairo_stroke(cr);
}

// A 1px outline whose outer edge is the given box.
void outline(cairo_t* cr, double x, double y, double w, double h, Color color)
{
    set_color(cr, color);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
    cairo_stroke(cr);
}

void triangle(cairo_t* cr, double cx, double cy, double half, bool up)
{
    set_color(cr, black);
    const double dir = up ? -1 : 1;
    cairo_move_to(cr, cx - half, cy - dir * half / 2);
    cairo_line_to(cr, cx + half, cy - dir * half / 2);
    cairo_line_to(cr, cx, cy + dir * half / 2 + dir);
    cairo_close_path(cr);
    cairo_fill(cr);
}

void paint_button(cairo_t* cr, double x, double y, bool up, bool down)
{
    fill(cr, x, y, button_size, button_size, face);
    bevel(cr, x, y, button_size, button_size, true);
    const double cx = x + button_size / 2.0;
    const double cy = y + button_size / 2.0;
    if (up && down) { // restore
        triangle(cr, cx, cy - 3, 4, true);
        triangle(cr, cx, cy + 3, 4, false);
    } else {
        triangle(cr, cx, cy, 5, up);
    }
}

void paint_control_box(cairo_t* cr, double x, double y)
{
    fill(cr, x, y, button_size, button_size, face);
    // The "space bar": a white bar with a shadow.
    const double bw = button_size - 8;
    const double bx = x + 4;
    const double by = y + button_size / 2.0 - 1;
    fill(cr, bx + 1, by + 1, bw, 3, shadow);
    fill(cr, bx, by, bw, 3, white);
    outline(cr, bx, by, bw, 3, black);
}

PangoLayout* make_layout(cairo_t* cr, std::string_view text, const char* font)
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
    return layout;
}

} // namespace

void set_color(cairo_t* cr, Color color)
{
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
}

void fill(cairo_t* cr, double x, double y, double w, double h, Color color)
{
    set_color(cr, color);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

void bevel(cairo_t* cr, double x, double y, double w, double h, bool raised)
{
    const Color light = raised ? white : shadow;
    const Color dark = raised ? shadow : white;
    // Two-pixel dark edge on raised boxes, as on classic buttons.
    line(cr, x + 0.5, y + h - 0.5, x + 0.5, y + 0.5, light);
    line(cr, x + 0.5, y + 0.5, x + w - 0.5, y + 0.5, light);
    line(cr, x + w - 0.5, y + 0.5, x + w - 0.5, y + h - 0.5, dark);
    line(cr, x + w - 0.5, y + h - 0.5, x + 0.5, y + h - 0.5, dark);
    if (raised) {
        line(cr, x + w - 1.5, y + 1.5, x + w - 1.5, y + h - 1.5, dark);
        line(cr, x + w - 1.5, y + h - 1.5, x + 1.5, y + h - 1.5, dark);
    }
}

void paint_frame(cairo_t* cr, int w, int h, std::string_view title, bool active, bool maximized,
    FrameButtons buttons, const Color* client)
{
    // The sizing border, with its black outer and inner lines.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    set_color(cr, face);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_rectangle(cr, border, frame_top, w - border - frame_right, h - frame_top - frame_bottom);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    outline(cr, 0, 0, w, h, black);
    outline(cr, border - 1, border - 1, w - 2 * (border - 1), h - 2 * (border - 1), black);

    // Notches marking where corner resizing starts.
    const double corner = title_height + border;
    if (!maximized) {
        line(cr, corner + 0.5, 0, corner + 0.5, border, black);
        line(cr, w - corner - 0.5, 0, w - corner - 0.5, border, black);
        line(cr, corner + 0.5, h - border, corner + 0.5, h, black);
        line(cr, w - corner - 0.5, h - border, w - corner - 0.5, h, black);
        line(cr, 0, corner + 0.5, border, corner + 0.5, black);
        line(cr, 0, h - corner - 0.5, border, h - corner - 0.5, black);
        line(cr, w - border, corner + 0.5, w, corner + 0.5, black);
        line(cr, w - border, h - corner - 0.5, w, h - corner - 0.5, black);
    }

    // Title bar.
    const TitleLayout l = title_layout(w, buttons);
    fill(cr, border, border, w - 2 * border, title_height - 1, active ? active_title : inactive_title);
    line(cr, border, frame_top - 0.5, w - border, frame_top - 0.5, black);

    if (l.control_x >= 0) {
        paint_control_box(cr, l.control_x, border);
        line(cr, l.control_x + button_size + 0.5, border, l.control_x + button_size + 0.5, frame_top - 1, black);
    }
    if (l.minimize_x >= 0) {
        line(cr, l.minimize_x - 0.5, border, l.minimize_x - 0.5, frame_top - 1, black);
        paint_button(cr, l.minimize_x, border, false, true);
    }
    if (l.maximize_x >= 0) {
        line(cr, l.maximize_x - 0.5, border, l.maximize_x - 0.5, frame_top - 1, black);
        paint_button(cr, l.maximize_x, border, true, maximized);
    }

    PangoLayout* layout = make_layout(cr, title, title_font);
    const int text_width = std::max(0, static_cast<int>(l.text_right - l.text_left) - 8);
    pango_layout_set_width(layout, text_width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    set_color(cr, active ? white : black);
    cairo_move_to(cr, l.text_left + 4, border + (title_height - 1 - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);

    if (client)
        fill(cr, frame_left, frame_top, w - frame_left - frame_right, h - frame_top - frame_bottom, *client);
}

FrameHit hit_test_frame(int w, int h, double x, double y, FrameButtons buttons, bool resizable)
{
    const int outside = resizable ? grab_margin : 0;
    if (x < -outside || y < -outside || x >= w + outside || y >= h + outside)
        return {};

    const bool on_border = x < border || x >= w - border || y < border || y >= h - border;
    if (on_border) {
        if (!resizable)
            return { FramePart::Title };
        const double corner = title_height + border;
        std::uint32_t edges = 0;
        if (x < border)
            edges |= WLR_EDGE_LEFT;
        if (x >= w - border)
            edges |= WLR_EDGE_RIGHT;
        if (y < border)
            edges |= WLR_EDGE_TOP;
        if (y >= h - border)
            edges |= WLR_EDGE_BOTTOM;
        // The notched corners resize in both directions.
        if (edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) {
            if (x < corner)
                edges |= WLR_EDGE_LEFT;
            if (x >= w - corner)
                edges |= WLR_EDGE_RIGHT;
        }
        if (edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
            if (y < corner)
                edges |= WLR_EDGE_TOP;
            if (y >= h - corner)
                edges |= WLR_EDGE_BOTTOM;
        }
        return { FramePart::Edge, edges };
    }

    if (y < frame_top) {
        const TitleLayout l = title_layout(w, buttons);
        if (l.control_x >= 0 && x < l.control_x + button_size)
            return { FramePart::Control };
        if (l.maximize_x >= 0 && x >= l.maximize_x)
            return { FramePart::Maximize };
        if (l.minimize_x >= 0 && x >= l.minimize_x && (l.maximize_x < 0 || x < l.maximize_x))
            return { FramePart::Minimize };
        return { FramePart::Title };
    }
    return { FramePart::Client };
}

int paint_label(cairo_t* cr, std::string_view text, double cx, double y, double width, bool selected,
    Color text_color)
{
    PangoLayout* layout = make_layout(cr, text, label_font);
    pango_layout_set_width(layout, static_cast<int>(width) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_height(layout, -2); // at most two lines

    PangoRectangle ink{}, logical{};
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    const double left = cx - width / 2.0;
    if (selected) {
        fill(cr, left + logical.x - 1, y, logical.width + 2, logical.height, selection);
        text_color = white;
    }
    set_color(cr, text_color);
    cairo_move_to(cr, left, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    return logical.height;
}

void paint_arrow_button(cairo_t* cr, double x, double y, double size, bool up)
{
    fill(cr, x, y, size, size, face);
    bevel(cr, x, y, size, size, true);
    triangle(cr, x + size / 2.0, y + size / 2.0, size / 4.0, up);
}

void paint_icon(cairo_t* cr, cairo_surface_t* surface, double x, double y, double size)
{
    if (!surface)
        return;
    const int iw = cairo_image_surface_get_width(surface);
    const int ih = cairo_image_surface_get_height(surface);
    if (iw <= 0 || ih <= 0)
        return;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    const double scale = size / std::max(iw, ih);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, (std::max(iw, ih) - iw) / 2.0, (std::max(iw, ih) - ih) / 2.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
}

} // namespace wfa::retro
