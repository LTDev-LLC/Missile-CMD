#include "render_internal.h"

#include "balance.h"
#include "collision.h"
#include <stdio.h>
#include <string.h>

#define MC_HUD_GLYPH_WIDTH   3U
#define MC_HUD_GLYPH_HEIGHT  5U
#define MC_HUD_GLYPH_STEP    (MC_HUD_GLYPH_WIDTH + 1U)
#define MC_HUD_HEIGHT        (MC_HUD_GLYPH_HEIGHT + 2U)
#define MC_GAMEPLAY_GROUND_Y (MC_SCREEN_HEIGHT - MC_HUD_GLYPH_HEIGHT - 2U)

// Original 3x5 XBM glyphs: digits, then letters in McHudLetters order
static const char McHudLetters[] = "ACHLORTW";
static const uint8_t McHudGlyphs[][MC_HUD_GLYPH_HEIGHT] = {
    {7, 5, 5, 5, 7}, // 0
    {2, 3, 2, 2, 7}, // 1
    {7, 4, 7, 1, 7}, // 2
    {7, 4, 7, 4, 7}, // 3
    {5, 5, 7, 4, 4}, // 4
    {7, 1, 7, 4, 7}, // 5
    {7, 1, 7, 5, 7}, // 6
    {7, 4, 2, 2, 2}, // 7
    {7, 5, 7, 5, 7}, // 8
    {7, 5, 7, 4, 7}, // 9
    {2, 5, 7, 5, 5}, // A
    {6, 1, 1, 1, 6}, // C
    {5, 5, 7, 5, 5}, // H
    {1, 1, 1, 1, 7}, // L
    {2, 5, 5, 5, 2}, // O
    {3, 5, 3, 5, 5}, // R
    {7, 2, 2, 2, 2}, // T
    {5, 5, 7, 7, 5}, // W
};

static void mc_draw_hud_text(Canvas* canvas, int16_t x, int16_t top, const char* text) {
    for(; *text && x <= MC_SCREEN_WIDTH - (int16_t)MC_HUD_GLYPH_WIDTH;
        text++, x += MC_HUD_GLYPH_STEP) {
        const char c = *text;
        const uint8_t* glyph = NULL;
        if(c >= '0' && c <= '9')
            glyph = McHudGlyphs[c - '0'];
        else {
            const char* letter = strchr(McHudLetters, c);
            if(letter) glyph = McHudGlyphs[10 + (letter - McHudLetters)];
        }
        if(glyph)
            canvas_draw_xbm(canvas, x, top, MC_HUD_GLYPH_WIDTH, MC_HUD_GLYPH_HEIGHT, glyph);
        else if(c == '-' || c == '+') {
            canvas_draw_line(canvas, x, top + 2, x + MC_HUD_GLYPH_WIDTH - 1, top + 2);
            if(c == '+') canvas_draw_line(canvas, x + 1, top + 1, x + 1, top + 3);
        }
    }
}

static const uint8_t McCitySprite[] = {
    0x10,
    0x00,
    0x7C,
    0x00,
    0xEE,
    0x00,
    0xFF,
    0x01,
    0xFF,
    0x01,
    0xD7,
    0x01,
    0xFF,
    0x01,
};
static const uint8_t McBatterySprite[] = {
    0x10,
    0x00,
    0x38,
    0x00,
    0x38,
    0x00,
    0x7C,
    0x00,
    0x6C,
    0x00,
    0xC6,
    0x00,
    0xFF,
    0x01,
    0xFF,
    0x01,
};
static const uint8_t McCityRubbleSprite[] = {
    0x00,
    0x00,
    0x82,
    0x00,
    0x28,
    0x00,
    0x7C,
    0x00,
    0xEE,
    0x00,
    0xFF,
    0x01,
    0xBB,
    0x01,
};
static const uint8_t McBatteryRubbleSprite[] = {
    0x00,
    0x00,
    0x60,
    0x00,
    0x36,
    0x00,
    0x78,
    0x00,
    0xF6,
    0x00,
    0xFF,
    0x01,
    0xBB,
    0x01,
};
static const uint8_t McMissileSprite[] = {0x02, 0x07, 0x02};
static const uint8_t McFastSprite[] = {0x05, 0x02, 0x05};
static const uint8_t McSplitterSprite[] = {0x07, 0x05, 0x07};

static void mc_draw_site_at_ground(
    Canvas* canvas,
    uint8_t site_index,
    bool alive,
    bool selected,
    int16_t ground_y) {
    const McSiteDef* site = mc_game_site_def(site_index);
    if(!site) return;
    const uint8_t* sprite =
        !alive ? (site->kind == McSiteBattery ? McBatteryRubbleSprite : McCityRubbleSprite) :
        site->kind == McSiteBattery ? McBatterySprite :
                                      McCitySprite;
    // Battery sprites are one row taller but share the ground anchor
    const uint8_t height = site->kind == McSiteBattery && alive ? 8U : 7U;
    const int16_t top = ground_y - height;
    canvas_draw_xbm(canvas, site->x - 4, top, 9U, height, sprite);
    if(selected) canvas_draw_frame(canvas, site->x - 6, top - 1, 13U, height + 2U);
}

void mc_render_site(Canvas* canvas, uint8_t site_index, bool alive, bool selected) {
    mc_draw_site_at_ground(canvas, site_index, alive, selected, MC_GROUND_Y);
}

static int16_t mc_abs_i16(int16_t value) {
    return value < 0 ? (int16_t)-value : value;
}

// Bresenham line rasterization, drawing alternate pixels
static void mc_draw_dashed_line(
    Canvas* canvas,
    int16_t x0,
    int16_t y0,
    int16_t x1,
    int16_t y1,
    uint8_t top,
    uint8_t bottom) {
    if((y0 < top && y1 < top) || (y0 > bottom && y1 > bottom)) return;
    const int16_t dx = mc_abs_i16((int16_t)(x1 - x0));
    const int16_t sx = x0 < x1 ? 1 : -1;
    const int16_t dy = (int16_t)-mc_abs_i16((int16_t)(y1 - y0));
    const int16_t sy = y0 < y1 ? 1 : -1;
    int16_t error = (int16_t)(dx + dy);
    uint8_t step = 0U;
    while(true) {
        if((step & 1U) == 0U && y0 >= top && y0 <= bottom) canvas_draw_dot(canvas, x0, y0);
        if(x0 == x1 && y0 == y1) break;
        // Both axes must use the same error sample for diagonal steps
        const int16_t twice_error = (int16_t)(2 * error);
        if(twice_error >= dy) {
            error = (int16_t)(error + dy);
            x0 = (int16_t)(x0 + sx);
        }
        if(twice_error <= dx) {
            error = (int16_t)(error + dx);
            y0 = (int16_t)(y0 + sy);
        }
        step++;
    }
}

static void mc_draw_enemy_head(Canvas* canvas, McEnemyKind kind, int16_t x, int16_t y) {
    const uint8_t* sprite = kind == McEnemySplitter ? McSplitterSprite :
                            kind == McEnemyFast     ? McFastSprite :
                                                      McMissileSprite;
    canvas_draw_xbm(canvas, x - 1, y - 1, 3U, 3U, sprite);
}

static void mc_draw_effect(
    Canvas* canvas,
    uint8_t x,
    uint8_t y,
    uint8_t radius,
    uint8_t age,
    bool hostile,
    bool reduced_flash,
    uint8_t top,
    uint8_t bottom) {
    if(radius == 0U || y + radius < top || (int16_t)y - radius > bottom) return;
    if(!reduced_flash && !hostile && age == MC_EXPLOSION_EXPAND_TICKS - 1U) {
        canvas_draw_disc(canvas, x, y, radius);
    } else {
        canvas_draw_circle(canvas, x, y, radius);
        if(hostile && radius > 2U) canvas_draw_line(canvas, x - radius, y, x + radius, y);
    }
}

void mc_render_gameplay(Canvas* canvas, const McRenderSnapshot* model) {
    const McGame* game = &model->game;
    const uint8_t ground_y = model->settings.simple_hud ? 54U : MC_GAMEPLAY_GROUND_Y;
    const uint8_t hud_height = model->settings.simple_hud ? 10U : MC_HUD_HEIGHT;
    canvas_set_font(canvas, FontSecondary);

    const uint8_t cursor_x = mc_game_cursor_x(game);
    const uint8_t cursor_y = mc_game_cursor_y(game);
    if(model->cached_selected_battery >= 0) {
        const McSiteDef* battery = mc_game_site_def((uint8_t)model->cached_selected_battery);
        if(battery && battery->kind == McSiteBattery) {
            mc_draw_dashed_line(
                canvas, battery->x, battery->y, cursor_x, cursor_y, hud_height, ground_y);
        }
    }

    if(model->aim_ticks) {
        canvas_draw_circle(canvas, cursor_x, cursor_y, game->balance.defensive_radius);
        const int16_t width = canvas_string_width(canvas, model->aim_text);
        const int16_t radius = game->balance.defensive_radius;
        const int16_t tx = cursor_x + radius + 3 + width < MC_SCREEN_WIDTH ?
                               cursor_x + radius + 3 :
                               cursor_x - radius - width - 3;
        const int16_t ty = cursor_y < hud_height + 10U ? cursor_y + 11 : cursor_y - 4;
        canvas_draw_str(canvas, tx, ty, model->aim_text);
    }
    // Consume copies of the masks; rendering must not remove simulation objects
    uint32_t enemies = game->enemy_mask;
    while(enemies != 0U) {
        const uint8_t i = mc_mask_take_first(&enemies);
        const McEnemyMissile* enemy = &game->enemies[i];
        const int16_t x = mc_path_x(&enemy->path);
        const int16_t y = mc_path_y(&enemy->path);
        if(model->settings.trail_density) {
            const int16_t tail_x = enemy->origin_x;
            const int16_t tail_y = enemy->origin_y;
            if(enemy->kind == McEnemyFast)
                mc_draw_dashed_line(canvas, tail_x, tail_y, x, y, hud_height, ground_y);
            else if(!((tail_y < hud_height && y < hud_height) ||
                      (tail_y > ground_y && y > ground_y)))
                canvas_draw_line(canvas, tail_x, tail_y, x, y);
            if(enemy->kind == McEnemySplitter &&
               !((tail_y < hud_height && y < hud_height) || (tail_y > ground_y && y > ground_y)))
                canvas_draw_line(canvas, tail_x + 1, tail_y, x + 1, y);
        }
        mc_draw_enemy_head(canvas, enemy->kind, x, y);
        if(enemy->kind == McEnemyEvasive) canvas_draw_frame(canvas, x - 2, y - 2, 5U, 5U);
    }

    uint32_t interceptors = game->interceptor_mask;
    while(interceptors != 0U) {
        const uint8_t i = mc_mask_take_first(&interceptors);
        const McInterceptor* interceptor = &game->interceptors[i];
        const int16_t x = mc_path_x(&interceptor->path);
        const int16_t y = mc_path_y(&interceptor->path);
        if(model->settings.trail_density) {
            const McSiteDef* battery = mc_game_site_def(interceptor->battery_site);
            if(battery)
                mc_draw_dashed_line(canvas, battery->x, battery->y, x, y, hud_height, ground_y);
        }
        canvas_draw_dot(canvas, x, y);
    }

    uint32_t roots = game->root_mask;
    while(roots != 0U) {
        const uint8_t i = mc_mask_take_first(&roots);
        const McRootExplosion* effect = &game->roots[i];
        mc_draw_effect(
            canvas,
            effect->x,
            effect->y,
            effect->radius,
            effect->age,
            false,
            model->settings.reduced_flash,
            hud_height,
            ground_y);
    }
    uint32_t chains = game->chain_mask;
    while(chains != 0U) {
        const uint8_t i = mc_mask_take_first(&chains);
        const McChainExplosion* effect = &game->chains[i];
        mc_draw_effect(
            canvas,
            effect->x,
            effect->y,
            effect->radius,
            effect->age,
            false,
            model->settings.reduced_flash,
            hud_height,
            ground_y);
    }
    uint32_t impacts = game->impact_mask;
    while(impacts != 0U) {
        const uint8_t i = mc_mask_take_first(&impacts);
        const McImpactEffect* effect = &game->impacts[i];
        mc_draw_effect(
            canvas,
            effect->x,
            effect->y,
            effect->radius,
            effect->age,
            true,
            model->settings.reduced_flash,
            hud_height,
            ground_y);
    }

    for(uint8_t i = 0U; i < MC_SITE_COUNT; i++) {
        mc_draw_site_at_ground(
            canvas,
            i,
            mc_game_site_alive(game, i),
            model->cached_selected_battery == (int8_t)i,
            ground_y);
    }
    canvas_draw_line(canvas, 0, ground_y, 127, ground_y);
    if(game->modifier == McWaveModifierFocusFire && game->priority_site < MC_SITE_COUNT) {
        const McSiteDef* target = mc_game_site_def(game->priority_site);
        canvas_draw_line(canvas, target->x - 3, ground_y - 13, target->x, ground_y - 10);
        canvas_draw_line(canvas, target->x, ground_y - 10, target->x + 3, ground_y - 13);
    }
    for(uint8_t site = 0U; site < MC_SITE_COUNT; site++) {
        if(!(model->warning_sites & (1U << site))) continue;
        const uint8_t x = mc_game_site_def(site)->x;
        canvas_draw_line(canvas, x, ground_y - 20, x, ground_y - 17);
        canvas_draw_dot(canvas, x, ground_y - 15);
    }
    if(model->settings.strong_cursor) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, cursor_x - 3, cursor_y - 3, 7U, 7U);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_frame(canvas, cursor_x - 3, cursor_y - 3, 7U, 7U);
    }
    canvas_draw_line(canvas, cursor_x - 3, cursor_y, cursor_x + 3, cursor_y);
    canvas_draw_line(canvas, cursor_x, cursor_y - 3, cursor_x, cursor_y + 3);

    // Draw opaque HUD backgrounds last so effects cannot obscure the readouts
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, 0, MC_SCREEN_WIDTH, hud_height);
    canvas_draw_box(canvas, 0, ground_y + 1, MC_SCREEN_WIDTH, MC_SCREEN_HEIGHT - ground_y - 1);
    canvas_set_color(canvas, ColorBlack);
    if(model->settings.simple_hud)
        canvas_draw_str(canvas, 1, 8, model->hud_text);
    else
        mc_draw_hud_text(canvas, 1, 0, model->hud_text);
    canvas_draw_line(canvas, 0, hud_height - 1, MC_SCREEN_WIDTH - 1, hud_height - 1);

    char footer[32];
    if(model->settings.simple_hud)
        canvas_draw_str(canvas, 1, 63, model->footer_text);
    else
        mc_draw_hud_text(canvas, 1, MC_SCREEN_HEIGHT - MC_HUD_GLYPH_HEIGHT, model->footer_text);
    if(game->mode == McModeSprint) canvas_draw_str(canvas, 103, 16, model->timer_text);
    if(model->battery_overlay) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 0, 36, 128, 19);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_frame(canvas, 0, 36, 128, 19);
        char ammo[3][4];
        for(uint8_t i = 0U; i < 3U; i++) {
            if(mc_game_site_alive(game, i * 3U))
                snprintf(ammo[i], sizeof(ammo[i]), "%u", game->battery_ammo[i]);
            else
                snprintf(ammo[i], sizeof(ammo[i]), "--");
        }
        snprintf(footer, sizeof(footer), "L:%s  Up:%s  R:%s", ammo[0], ammo[1], ammo[2]);
        mc_render_centered(canvas, 37, footer);
        mc_render_centered(canvas, 45, "Down Auto / release OK");
    }
    if(model->shot_notice_ticks || model->countdown_ticks) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 10, 21, 108, 12);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_frame(canvas, 10, 21, 108, 12);
        if(model->countdown_ticks)
            snprintf(footer, sizeof(footer), "Ready... %u", (model->countdown_ticks + 29U) / 30U);
        else
            snprintf(footer, sizeof(footer), "%s", mc_fire_reason(model->shot_reason));
        mc_render_centered(canvas, 23, footer);
    }

    if(model->storage_notice_visible) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 35, 10, 58, 10);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_frame(canvas, 35, 10, 58, 10);
        canvas_draw_str_aligned(
            canvas,
            64,
            11,
            AlignCenter,
            AlignTop,
            model->storage_recovered ? "Recovered" : "Save error");
    }
}
