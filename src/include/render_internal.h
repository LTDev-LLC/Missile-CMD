#pragma once

#include "render.h"

const char* mc_render_difficulty_name(McDifficulty difficulty);
void mc_render_centered(Canvas* canvas, int32_t y, const char* text);
void mc_render_menu_item(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    size_t width,
    const char* text,
    bool selected);
void mc_render_site(Canvas* canvas, uint8_t site_index, bool alive, bool selected);
void mc_render_gameplay(Canvas* canvas, const McRenderSnapshot* model);
void mc_render_screen(Canvas* canvas, const McRenderSnapshot* model);

void mc_render_printf(Canvas* canvas, int32_t y, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

void mc_render_heading(Canvas* canvas, int y, const char* text);
void mc_render_row(Canvas* canvas, int y, const char* text, bool selected);

void mc_render_lines(Canvas* canvas, int x, int y, int step, uint8_t count, const char* text);
void mc_render_help(Canvas* canvas, const McRenderSnapshot* model, int x, int y, int step);
