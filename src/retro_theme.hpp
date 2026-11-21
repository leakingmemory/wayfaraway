#pragma once

#include <cstdint>
#include <string_view>

#include <cairo.h>

namespace wfa::retro {

// A 16-colour-era palette: grey 3D faces, navy active title bars.
struct Color {
    double r, g, b;
};

inline constexpr Color black{ 0.0, 0.0, 0.0 };
inline constexpr Color white{ 1.0, 1.0, 1.0 };
inline constexpr Color face{ 0.75, 0.75, 0.75 };     // buttons, frames
inline constexpr Color shadow{ 0.5, 0.5, 0.5 };      // bevel shadow
inline constexpr Color desktop{ 0.0, 0.5, 0.5 };     // desktop background
inline constexpr Color workspace{ 1.0, 1.0, 1.0 };   // inside launcher windows
inline constexpr Color active_title{ 0.0, 0.0, 0.5 };
inline constexpr Color inactive_title{ 1.0, 1.0, 1.0 };
inline constexpr Color selection{ 0.0, 0.0, 0.5 };

inline constexpr const char* title_font = "Sans Bold 9";
inline constexpr const char* label_font = "Sans 8";

// Window frame geometry around a client area.
inline constexpr int border = 4;        // resizing frame
inline constexpr int title_height = 20; // includes its bottom line
inline constexpr int frame_left = border;
inline constexpr int frame_right = border;
inline constexpr int frame_top = border + title_height;
inline constexpr int frame_bottom = border;

// Which title-bar controls a frame has.
struct FrameButtons {
    bool control = true;  // the control-menu box at the left
    bool minimize = true;
    bool maximize = true;
};

enum class FramePart { None, Client, Title, Control, Minimize, Maximize, Edge };

struct FrameHit {
    FramePart part = FramePart::None;
    std::uint32_t edges = 0; // WLR_EDGE_* for FramePart::Edge
};

void set_color(cairo_t* cr, Color color);
void fill(cairo_t* cr, double x, double y, double w, double h, Color color);
// A 3D box: light top-left and dark bottom-right edges when raised.
void bevel(cairo_t* cr, double x, double y, double w, double h, bool raised);

// Paints a full window frame of outer size w x h. The client area is left
// untouched (transparent) unless `client` is given.
void paint_frame(cairo_t* cr, int w, int h, std::string_view title, bool active, bool maximized,
    FrameButtons buttons, const Color* client = nullptr);

// Hit-tests a point in frame-local coordinates (0,0 = outer top-left).
FrameHit hit_test_frame(int w, int h, double x, double y, FrameButtons buttons, bool resizable);

// An icon caption: centred on cx, wrapped to `width`, at most two lines.
// Returns its height.
int paint_label(cairo_t* cr, std::string_view text, double cx, double y, double width, bool selected,
    Color text_color = black);

// A raised square button with an up or down arrow (scrollbars).
void paint_arrow_button(cairo_t* cr, double x, double y, double size, bool up);

// Draws `surface` scaled into a size x size square at (x, y).
void paint_icon(cairo_t* cr, cairo_surface_t* surface, double x, double y, double size);

} // namespace wfa::retro
