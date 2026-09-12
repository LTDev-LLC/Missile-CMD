// Host implementation of the Canvas subset used by the application's real renderer
#include <gui/canvas.h>
#include "host_fonts.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Track bit-level glyph reads without unpacking the bundled font tables into extra buffers
typedef struct {
    const uint8_t* bytes;
    size_t bit;
} HostFontBits;

typedef struct {
    uint8_t width, height;
    int8_t x, y, advance;
    HostFontBits bits;
} HostGlyph;

// Select one of the bundled firmware font tables and reject unsupported font choices
static const uint8_t* host_font(const Canvas* canvas) {
    assert(canvas->font == FontPrimary || canvas->font == FontSecondary);
    return canvas->font == FontPrimary ? HostFontPrimary : HostFontSecondary;
}

// Read a packed font field least-significant bit first and advance the bit cursor
static uint8_t host_bits(HostFontBits* reader, uint8_t count) {
    uint8_t value = 0U;
    for(uint8_t i = 0; i < count; i++, reader->bit++)
        value |= ((reader->bytes[reader->bit / 8U] >> (reader->bit % 8U)) & 1U) << i;
    return value;
}

// Decode a signed font metric by subtracting the format's midpoint bias
static int8_t host_signed_bits(HostFontBits* reader, uint8_t count) {
    return (int8_t)((int)host_bits(reader, count) - (1 << (count - 1U)));
}

// Find a printable glyph and decode its size, offsets, advance, and bitmap cursor
static HostGlyph host_glyph(const uint8_t* font, uint8_t character) {
    /* Both bundled tables contain printable ASCII only */
    assert(character >= 32U && character <= 126U);
    // Glyph records follow the fixed font header and carry their own byte lengths
    const uint8_t* data = font + 23U;
    while(data[1] != 0U && data[0] != character)
        data += data[1];
    assert(data[1] != 0U);
    HostGlyph glyph = {.bits = {.bytes = data + 2U}};
    glyph.width = host_bits(&glyph.bits, font[4]);
    glyph.height = host_bits(&glyph.bits, font[5]);
    glyph.x = host_signed_bits(&glyph.bits, font[6]);
    glyph.y = host_signed_bits(&glyph.bits, font[7]);
    glyph.advance = host_signed_bits(&glyph.bits, font[8]);
    return glyph;
}

// Clear the monochrome framebuffer while preserving the selected font and color
__attribute__((weak)) void host_render_begin(void) {
}
void canvas_clear(Canvas* canvas) {
    host_render_begin();
    memset(canvas->pixels, 0, sizeof(canvas->pixels));
}

// Select the bundled font used by subsequent host text measurements and drawing
void canvas_set_font(Canvas* canvas, Font font) {
    canvas->font = font;
}

// Select whether subsequent pixels add, erase, or invert foreground ink
void canvas_set_color(Canvas* canvas, Color color) {
    canvas->color = color;
}

// Write one monochrome pixel only when its coordinates lie inside the display
void canvas_draw_dot(Canvas* canvas, int32_t x, int32_t y) {
    if(x >= 0 && x < 128 && y >= 0 && y < 64) {
        if(canvas->color == ColorXOR)
            canvas->pixels[y][x] ^= 1U;
        else
            canvas->pixels[y][x] = canvas->color == ColorBlack;
    }
}

// Sum glyph advances using the same metrics as host text drawing
uint16_t canvas_string_width(Canvas* canvas, const char* text) {
    uint16_t width = 0U;
    for(; *text; text++)
        width += host_glyph(host_font(canvas), (uint8_t)*text).advance;
    return width;
}

// Decode glyph runs and draw foreground pixels relative to the requested baseline
void canvas_draw_str(Canvas* canvas, int32_t x, int32_t baseline, const char* text) {
    const uint8_t* font = host_font(canvas);
    for(; *text; text++) {
        HostGlyph glyph = host_glyph(font, (uint8_t)*text);
        const size_t count = glyph.width * glyph.height;
        size_t pixel = 0U;
        while(pixel < count) {
            // Glyphs alternate background and foreground runs followed by a repeat bit
            const uint8_t zeros = host_bits(&glyph.bits, font[2]);
            const uint8_t ones = host_bits(&glyph.bits, font[3]);
            assert(zeros + ones != 0U);
            bool repeat;
            do {
                pixel += zeros;
                for(uint8_t i = 0U; i < ones && pixel < count; i++, pixel++)
                    canvas_draw_dot(
                        canvas,
                        x + glyph.x + (int32_t)(pixel % glyph.width),
                        baseline - glyph.height - glyph.y + (int32_t)(pixel / glyph.width));
                repeat = host_bits(&glyph.bits, 1U) != 0U;
            } while(repeat && pixel < count);
        }
        x += glyph.advance;
    }
}

// Translate alignment anchors into the font baseline before drawing text
void canvas_draw_str_aligned(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    Align horizontal,
    Align vertical,
    const char* text) {
    if(horizontal == AlignRight) x -= canvas_string_width(canvas, text);
    if(horizontal == AlignCenter) x -= canvas_string_width(canvas, text) / 2;
    // Vertical anchors use the font ascent so alignment matches baseline-based drawing
    const int8_t ascent = (int8_t)host_font(canvas)[13];
    if(vertical == AlignTop) y += ascent;
    if(vertical == AlignCenter) y += ascent / 2;
    canvas_draw_str(canvas, x, y, text);
}

// Rasterize an inclusive line with integer error steps and per-pixel clipping
void canvas_draw_line(Canvas* canvas, int32_t x, int32_t y, int32_t x2, int32_t y2) {
    const int32_t dx = abs(x2 - x), sx = x < x2 ? 1 : -1;
    const int32_t dy = -abs(y2 - y), sy = y < y2 ? 1 : -1;
    int32_t error = dx + dy;
    while(true) {
        canvas_draw_dot(canvas, x, y);
        if(x == x2 && y == y2) break;
        const int32_t twice = error * 2;
        if(twice >= dy) {
            error += dy;
            x += sx;
        }
        if(twice <= dx) {
            error += dx;
            y += sy;
        }
    }
}

// Fill a rectangle through the shared clipped pixel writer
void canvas_draw_box(Canvas* canvas, int32_t x, int32_t y, size_t width, size_t height) {
    for(size_t row = 0U; row < height; row++)
        for(size_t column = 0U; column < width; column++)
            canvas_draw_dot(canvas, x + (int32_t)column, y + (int32_t)row);
}

// Draw a rectangle outline, treating empty dimensions as a no-op
void canvas_draw_frame(Canvas* canvas, int32_t x, int32_t y, size_t width, size_t height) {
    if(!width || !height) return;
    const int32_t right = x + (int32_t)width - 1, bottom = y + (int32_t)height - 1;
    canvas_draw_line(canvas, x, y, right, y);
    canvas_draw_line(canvas, x, bottom, right, bottom);
    canvas_draw_line(canvas, x, y, x, bottom);
    canvas_draw_line(canvas, right, y, right, bottom);
}

// Rasterize symmetric circle points or spans with the integer midpoint algorithm
static void host_circle(Canvas* canvas, int32_t cx, int32_t cy, size_t radius, bool filled) {
    int32_t x = 0, y = (int32_t)radius, error = 1 - (int32_t)radius;
    while(x <= y) {
        for(int sign = -1; sign <= 1; sign += 2) {
            if(filled) {
                canvas_draw_line(canvas, cx - x, cy + sign * y, cx + x, cy + sign * y);
                canvas_draw_line(canvas, cx - y, cy + sign * x, cx + y, cy + sign * x);
            } else {
                canvas_draw_dot(canvas, cx - x, cy + sign * y);
                canvas_draw_dot(canvas, cx + x, cy + sign * y);
                canvas_draw_dot(canvas, cx - y, cy + sign * x);
                canvas_draw_dot(canvas, cx + y, cy + sign * x);
            }
        }
        x++;
        if(error < 0) {
            error += 2 * x + 1;
        } else {
            y--;
            error += 2 * (x - y) + 1;
        }
    }
}

// Draw an unfilled circular outline using the shared midpoint rasterizer
void canvas_draw_circle(Canvas* canvas, int32_t x, int32_t y, size_t radius) {
    host_circle(canvas, x, y, radius, false);
}

// Draw a filled circle using the shared midpoint rasterizer
void canvas_draw_disc(Canvas* canvas, int32_t x, int32_t y, size_t radius) {
    host_circle(canvas, x, y, radius, true);
}

// Draw an opaque row-padded bitmap and restore the original foreground color afterward
void canvas_draw_xbm(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    size_t width,
    size_t height,
    const uint8_t* bitmap) {
    // Round each bitmap row to whole bytes instead of letting unused tail bits enter the next row
    const size_t stride = (width + 7U) / 8U;
    const Color foreground = canvas->color;
    for(size_t row = 0U; row < height; row++)
        for(size_t column = 0U; column < width; column++) {
            const bool bit = (bitmap[row * stride + column / 8U] >> (column % 8U)) & 1U;
            canvas->color = bit ? foreground :
                                  (foreground == ColorBlack ? ColorWhite : ColorBlack);
            canvas_draw_dot(canvas, x + (int32_t)column, y + (int32_t)row);
        }
    canvas->color = foreground;
}
