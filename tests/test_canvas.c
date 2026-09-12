// Framebuffer and font invariants required for trustworthy host screenshots
#include <gui/canvas.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Check clipping, row padding, opaque bitmap backgrounds, and foreground erasure
static void test_clipping_and_bitmap_stride(void) {
    Canvas canvas = {0};
    canvas_set_color(&canvas, ColorBlack);
    canvas_draw_box(&canvas, -2, -2, 4, 4);
    canvas_draw_line(&canvas, 126, 63, 131, 63);
    size_t ink = 0;
    for(size_t y = 0; y < 64; y++)
        for(size_t x = 0; x < 128; x++)
            ink += canvas.pixels[y][x];
    assert(ink == 6U);
    canvas_clear(&canvas);
    canvas_draw_box(&canvas, 10, 10, 9, 2);
    // A nine-pixel row needs two bytes, exposing incorrect packed-row stride calculations
    const uint8_t bitmap[] = {0x01, 0x01, 0x02, 0x00};
    canvas_draw_xbm(&canvas, 10, 10, 9, 2, bitmap);
    assert(canvas.pixels[10][10] && canvas.pixels[10][18] && canvas.pixels[11][11]);
    assert(!canvas.pixels[10][11] && !canvas.pixels[11][18]);
    canvas_set_color(&canvas, ColorWhite);
    canvas_draw_dot(&canvas, 10, 10);
    assert(!canvas.pixels[10][10]);
}

// Check shared text metrics, alignment, case distinctions, and printable glyph coverage
static void test_font_metrics_alignment_and_case(void) {
    Canvas left = {0}, aligned = {0}, uppercase = {0};
    for(Font font = FontPrimary; font <= FontSecondary; font++) {
        canvas_clear(&left);
        canvas_clear(&aligned);
        canvas_clear(&uppercase);
        canvas_set_font(&left, font);
        canvas_set_font(&aligned, font);
        canvas_set_font(&uppercase, font);
        canvas_set_color(&left, ColorBlack);
        canvas_set_color(&aligned, ColorBlack);
        canvas_set_color(&uppercase, ColorBlack);
        /* Reference advances for these firmware bitmap fonts */
        const uint16_t width = font == FontPrimary ? 55U : 43U;
        assert(canvas_string_width(&left, "FinVM 1bit") == width);
        canvas_draw_str(&left, 64 - width / 2, 24, "FinVM 1bit");
        canvas_draw_str_aligned(&aligned, 64, 24, AlignCenter, AlignBottom, "FinVM 1bit");
        assert(memcmp(left.pixels, aligned.pixels, sizeof(left.pixels)) == 0);
        canvas_draw_str(&uppercase, 64 - width / 2, 24, "FINVM 1BIT");
        assert(memcmp(left.pixels, uppercase.pixels, sizeof(left.pixels)) != 0);
        for(unsigned char c = 32U; c <= 126U; c++) {
            const char text[] = {(char)c, '\0'};
            canvas_draw_str(&left, -1, 64, text);
        }
    }
}

// Run the host framebuffer and font checks used by both tests and screenshot capture
int main(void) {
    test_clipping_and_bitmap_stride();
    test_font_metrics_alignment_and_case();
    puts("host canvas clipping, bitmap, and font tests passed");
    return 0;
}
