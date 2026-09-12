#pragma once

// Monochrome host framebuffer and the Canvas API subset used by production drawing code

#include <stddef.h>
#include <stdint.h>
// Store one byte per pixel for straightforward assertions and deterministic PGM output
typedef struct Canvas {
    uint8_t font;
    uint8_t color;
    uint8_t pixels[64][128];
} Canvas;
typedef enum {
    FontPrimary,
    FontSecondary
} Font;
typedef enum {
    ColorWhite,
    ColorBlack,
    ColorXOR
} Color;
typedef enum {
    AlignLeft,
    AlignRight,
    AlignTop,
    AlignBottom,
    AlignCenter
} Align;
// Clear the monochrome framebuffer while preserving the selected font and color
void canvas_clear(Canvas* c);
// Select the bundled font used by subsequent host text measurements and drawing
void canvas_set_font(Canvas* c, Font font);
// Select whether subsequent pixels add or erase foreground ink
void canvas_set_color(Canvas* c, Color color);
// Sum glyph advances using the same metrics as host text drawing
uint16_t canvas_string_width(Canvas* c, const char* text);
// Decode glyph runs and draw foreground pixels relative to the requested baseline
void canvas_draw_str(Canvas* c, int32_t x, int32_t y, const char* text);
// Translate alignment anchors into the font baseline before drawing text
void canvas_draw_str_aligned(Canvas* c, int32_t x, int32_t y, Align h, Align v, const char* text);
// Rasterize an inclusive line with integer error steps and per-pixel clipping
void canvas_draw_line(Canvas* c, int32_t x, int32_t y, int32_t x2, int32_t y2);
// Write one monochrome pixel only when its coordinates lie inside the display
void canvas_draw_dot(Canvas* c, int32_t x, int32_t y);
// Fill a rectangle through the shared clipped pixel writer
void canvas_draw_box(Canvas* c, int32_t x, int32_t y, size_t w, size_t h);
// Draw a rectangle outline, treating empty dimensions as a no-op
void canvas_draw_frame(Canvas* c, int32_t x, int32_t y, size_t w, size_t h);
// Draw an unfilled circular outline using the shared midpoint rasterizer
void canvas_draw_circle(Canvas* c, int32_t x, int32_t y, size_t radius);
// Draw a filled circle using the shared midpoint rasterizer
void canvas_draw_disc(Canvas* c, int32_t x, int32_t y, size_t radius);
// Draw an opaque row-padded bitmap and restore the original foreground color afterward
void canvas_draw_xbm(Canvas* c, int32_t x, int32_t y, size_t w, size_t h, const uint8_t* image);
