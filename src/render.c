#include "help_ui.h"
#include "text.h"
#include "render_internal.h"
#include "persistence.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

const char* mc_render_difficulty_name(McDifficulty difficulty) {
    switch(difficulty) {
    case McDifficultyCadet:
        return "Cadet";
    case McDifficultyCrisis:
        return "Crisis";
    case McDifficultyCommand:
    default:
        return "Command";
    }
}

__attribute__((noinline)) void mc_render_centered(Canvas* canvas, int32_t y, const char* text) {
    canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignTop, text);
}

__attribute__((noinline)) void mc_render_menu_item(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    size_t width,
    const char* text,
    bool selected) {
    if(selected) {
        canvas_draw_box(canvas, x, y - 8, width, 9);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str(canvas, x + 3, y, text);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_str(canvas, x + 3, y, text);
    }
}

void mc_render(Canvas* canvas, const McRenderSnapshot* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    if(model->screen == McScreenPlaying) {
        mc_render_gameplay(canvas, model);
    } else {
        mc_render_screen(canvas, model);
    }

    if(!model->exit_pending && model->storage_pending && model->storage_result == McStorageBusy &&
       model->screen != McScreenPlaying && model->screen != McScreenLoading &&
       model->screen != McScreenCleanup && model->screen != McScreenStorage) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 0, 54, 128, 10);
        canvas_set_color(canvas, ColorBlack);
        canvas_set_font(canvas, FontSecondary);
        mc_render_printf(canvas, 55, "Working: %s", model->storage_item);
    }

    // Gameplay draws its notice below the HUD; other screens use this overlay
    if(model->storage_notice_visible && model->screen != McScreenPlaying &&
       model->screen != McScreenCleanup) {
        canvas_set_font(canvas, FontSecondary);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 82, 0, 46, 9);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_frame(canvas, 82, 0, 46, 9);
        canvas_draw_str_aligned(
            canvas,
            105,
            1,
            AlignCenter,
            AlignTop,
            model->storage_recovered ? "Recovered" : "Save error");
    }
    // Invert the completed frame so menus, gameplay and every overlay agree.
    if(model->settings.invert_colors) {
        canvas_set_color(canvas, ColorXOR);
        canvas_draw_box(canvas, 0, 0, 128, 64);
        canvas_set_color(canvas, ColorBlack);
    }
}

__attribute__((noinline)) void
    mc_render_printf(Canvas* canvas, int32_t y, const char* format, ...) {
    char text[48];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    mc_render_centered(canvas, y, text);
}

__attribute__((noinline)) void mc_render_heading(Canvas* canvas, int y, const char* text) {
    canvas_set_font(canvas, FontPrimary);
    mc_render_centered(canvas, y, text);
    canvas_set_font(canvas, FontSecondary);
}
void mc_render_row(Canvas* canvas, int y, const char* text, bool selected) {
    mc_render_menu_item(canvas, 3, y, 122, text, selected);
}

void mc_render_snapshot(McRenderSnapshot* snapshot, const McUiModel* model) {
    snapshot->common = model->common;
    snapshot->game = model->game;
    if(model->screen != McScreenPlaying) {
        snapshot->scores = model->scores;
        snapshot->profile = model->profile;
    }
}

void mc_render_lines(Canvas* canvas, int x, int y, int step, uint8_t count, const char* text) {
    for(uint8_t i = 0; i < count; i++) {
        if(x < 0)
            mc_render_centered(canvas, y, text);
        else
            canvas_draw_str(canvas, x, y, text);
        text += strlen(text) + 1U;
        y += step;
    }
}
void mc_render_help(Canvas* canvas, const McRenderSnapshot* model, int x, int y, int step) {
    const McHelpPage* page = &model->help;
    const uint8_t wanted = mc_help_requested(&model->common, &model->game);
    if(wanted != McHelpNoPage && page->id == wanted && page->status == McHelpReady) {
        mc_render_lines(canvas, x, y, step, page->lines, page->text);
    } else {
        mc_render_centered(
            canvas,
            y,
            page->id == wanted && page->status == McHelpUnavailable ? "Help unavailable" :
                                                                      "Loading help...");
        if(model->screen == McScreenLesson) {
            const uint8_t lesson = model->game.wave > 4U ? 3U :
                                   model->game.wave      ? model->game.wave - 1U :
                                                           0U;
            mc_render_centered(
                canvas,
                y + step,
                mc_text_at(
                    "Fire ahead of missiles\0Chain nearby missiles\0Use another battery\0Repair + buy ammo/radius",
                    lesson));
        }
    }
}
