#include "controls.h"
#include "render_internal.h"
#include "text.h"

#include "balance.h"
#include "ui_menu.h"
#include "wave.h"

#include <datetime/datetime.h>

#include <stdio.h>
#include <string.h>

// Immutable descriptors encode object offset and integer width; no pointer relocations.
#define MC_INFO_FIELD(type, field) \
    ((uint16_t)(offsetof(type, field) | (sizeof(((type*)0)->field) << 12)))
static uint32_t mc_info_value(const void* object, uint16_t field) {
    const unsigned char* bytes = (const unsigned char*)object + (field & 4095U);
    if((field >> 12) == 4) {
        uint32_t value;
        memcpy(&value, bytes, sizeof(value));
        return value;
    }
    if((field >> 12) == 2) {
        uint16_t value;
        memcpy(&value, bytes, sizeof(value));
        return value;
    }
    return *bytes;
}
_Static_assert(sizeof(McProfile) < 4096 && sizeof(McUiCommon) < 4096, "info offsets fit");

enum {
    McInfoRecords,
    McInfoPractice,
    McInfoBonus
};
static const struct {
    uint8_t y, step, rows;
} McInfoLayouts[] = {
    [McInfoRecords] = {19, 10, 4},
    [McInfoPractice] = {20, 9, 4},
    [McInfoBonus] = {15, 10, 4},
};

static void mc_draw_scroll_arrows(Canvas* canvas, uint8_t first, uint8_t visible, uint8_t count) {
    if(first > 0U) {
        canvas_draw_line(canvas, 124, 14, 126, 12);
        canvas_draw_line(canvas, 126, 12, 127, 14);
    }
    if(first + visible < count) {
        canvas_draw_line(canvas, 124, 61, 126, 63);
        canvas_draw_line(canvas, 126, 63, 127, 61);
    }
}

// Menu geometry stays numeric; screen-specific headings, scores and footers stay explicit.
static void mc_draw_menu_rows(Canvas* canvas, const McRenderSnapshot* model, McMenuKind menu) {
    static const struct {
        uint8_t y, step, limit;
    } layouts[] = {
        [McMenuTitle] = {19, 9, MC_MENU_VISIBLE_ITEMS},
        [McMenuPause] = {19, 9, MC_MENU_VISIBLE_ITEMS - 1U},
        [McMenuGameOver] = {29, 10, 4},
    };
    const uint8_t count = mc_menu_count(menu, mc_menu_context(&model->common, &model->game));
    const uint8_t visible = count < layouts[menu].limit ? count : layouts[menu].limit;
    const uint8_t first = mc_list_first(model->menu_index, visible);
    for(uint8_t row = 0; row < visible; row++) {
        const uint8_t index = first + row;
        const McMenuAction action =
            mc_menu_action(menu, mc_menu_context(&model->common, &model->game), index);
        const char* label =
            action == McMenuActionSaveTitle &&
                    !mc_session_has(model->game.mode, model->wave_practice, McModeSave) ?
                "End session" :
                mc_menu_label(action);
        mc_render_row(
            canvas, layouts[menu].y + row * layouts[menu].step, label, model->menu_index == index);
    }
    mc_draw_scroll_arrows(canvas, first, visible, count);
}
static void mc_draw_menu(
    Canvas* canvas,
    const McRenderSnapshot* model,
    McMenuKind menu,
    const char* title) {
    mc_render_heading(canvas, 1, title);
    mc_draw_menu_rows(canvas, model, menu);
}

static void mc_draw_hud_guide(Canvas* canvas, const McRenderSnapshot* model) {
    mc_render_heading(canvas, 1, model->guide_page ? "BOTTOM 2/2" : "TOP 1/2");
    mc_render_help(canvas, model, 3, 20, 8);
    mc_render_centered(canvas, 56, "<>/OK Page Back");
}

static void mc_draw_control_card(Canvas* canvas) {
    mc_render_heading(canvas, 1, "CONTROLS");
    mc_render_lines(
        canvas,
        8,
        20,
        10,
        4,
        "D-pad  Move crosshair\0OK     Fire interceptor\0Back   Pause\0Hold OK + D-pad: battery");
    mc_render_centered(canvas, 56, "OK Start  Back Cancel");
}

static void mc_draw_wave_result(Canvas* canvas, const McRenderSnapshot* model) {
    char text[40];
    if(model->stats_page) {
        const McGame* g = &model->game;
        mc_render_heading(canvas, 1, model->stats_page == 1U ? "WAVE STATS" : "BONUS BREAKDOWN");
        if(model->stats_page == 1U) {
            mc_render_printf(
                canvas,
                15,
                "Shots %u   Hits %u%%",
                g->wave_stats.shots,
                mc_accuracy_x100(g->wave_stats.hits, g->wave_stats.shots) / 100U);
            mc_render_printf(
                canvas, 25, "Intercepted %u  Chain %u", g->wave_stats.kills, g->wave_stats.chain);
            uint8_t lost = 0U;
            for(uint8_t i = 0U; i < MC_SITE_COUNT; i++)
                if((g->wave_start_alive_mask & (1U << i)) && !mc_game_site_alive(g, i)) lost++;
            mc_render_printf(canvas, 35, "Sites lost %u", lost);
            if(model->pace_available)
                mc_render_printf(canvas, 45, "Best-run pace: %+ld", (long)model->pace_delta);
            else
                mc_render_centered(canvas, 45, "No matching pace yet");
        } else {
            const uint32_t values[] = {
                (uint32_t)g->wave * 100U,
                mc_game_alive_cities(g) * 100U,
                mc_game_total_ammo(g) * 5U,
                g->alive_site_mask == g->wave_start_alive_mask ? 500U : 0U};
            const char* format = "Wave: %lu\0Cities: %lu\0Ammo: %lu\0Perfect: %lu";
            for(uint8_t row = 0; row < McInfoLayouts[McInfoBonus].rows; row++) {
                mc_render_printf(
                    canvas,
                    McInfoLayouts[McInfoBonus].y + row * McInfoLayouts[McInfoBonus].step,
                    format,
                    (unsigned long)values[row]);
                format = mc_text_at(format, 1);
            }
        }
        mc_render_centered(canvas, 55, "<> Pages  OK Continue");
        return;
    }
    mc_render_heading(
        canvas,
        3,
        model->game.mode == McModeTraining ?
            (!mc_training_passed(&model->game) ? "TRY LESSON AGAIN" :
             model->game.wave >= 4U            ? "TRAINING COMPLETE" :
                                                 "LESSON CLEARED") :
            "WAVE CLEARED");
    mc_render_printf(
        canvas, 15, "W%03u Bonus %05u", model->game.wave, model->game.last_wave_bonus);
    mc_render_printf(
        canvas,
        25,
        "%s / %s",
        mc_wave_modifier_name(model->game.next_modifier),
        mc_wave_pattern_name(
            model->game.mode == McModeTraining ?
                (model->game.wave == 1U ? McWavePatternSalvo : McWavePatternStaggered) :
                mc_wave_pattern(
                    model->game.wave == UINT16_MAX ? UINT16_MAX : model->game.wave + 1U)));
    mc_render_centered(
        canvas,
        35,
        mc_wave_modifier_effect(model->game.next_modifier, (uint16_t)(model->game.wave + 1U)));
    if(model->medal_notice_mask != 0U) {
        uint8_t first = 0U;
        uint8_t count = 0U;
        for(uint8_t i = 0U; i < McMedalCount; i++) {
            if((model->medal_notice_mask & (1U << i)) == 0U) continue;
            if(count == 0U) first = i;
            count++;
        }
        if(count > 1U)
            snprintf(text, sizeof(text), "Medal: %s +%u", mc_medal_name(first), count - 1U);
        else
            snprintf(text, sizeof(text), "Medal: %s", mc_medal_name(first));
    } else {
        snprintf(
            text,
            sizeof(text),
            "Cities %u Ammo %u Repair %u",
            mc_game_alive_cities(&model->game),
            mc_game_total_ammo(&model->game),
            model->game.repair_credits);
    }
    mc_render_centered(canvas, 45, text);
    mc_render_centered(canvas, 55, "<> Stats  OK Continue");
}

static void mc_draw_repair(Canvas* canvas, const McRenderSnapshot* model) {
    char text[32];
    mc_render_heading(canvas, 1, "SUPPLIES");
    mc_render_printf(canvas, 15, "Credits: %u", model->game.repair_credits);
    const McSiteDef* selected = mc_game_site_def(model->repair_site_index);
    if(model->supply_index == 0U)
        snprintf(
            text,
            sizeof(text),
            "< %s %u >  Repair: 1",
            selected && selected->kind == McSiteBattery ? "Battery" : "City",
            selected && selected->kind == McSiteBattery ?
                model->repair_site_index / 3U + 1U :
                model->repair_site_index - model->repair_site_index / 3U);
    else if(model->supply_index == 1U)
        snprintf(
            text, sizeof(text), "+3 ammo/base: %s", model->game.bonus_ammo ? "Bought" : "1 credit");
    else if(model->supply_index == 2U)
        snprintf(
            text,
            sizeof(text),
            "+2 blast radius: %s",
            model->game.next_radius_boost ? "Bought" : "1 credit");
    else
        snprintf(
            text,
            sizeof(text),
            "%s: %s",
            model->supply_index == 3U ? "+50% shot speed" : "Longer blast hold",
            model->game.next_supplies & (1U << (model->supply_index - 3U)) ? "Bought" :
                                                                             "1 credit");
    mc_render_centered(canvas, 27, text);
    mc_render_centered(
        canvas,
        35,
        model->workshop_reason ? mc_workshop_reason(model->workshop_reason) :
                                 "Up/Down type  OK Buy");
    for(size_t i = 0U; i < MC_SITE_COUNT; i++) {
        mc_render_site(
            canvas, i, mc_game_site_alive(&model->game, i), model->repair_site_index == i);
    }
    canvas_draw_line(canvas, 0, MC_GROUND_Y, 127, MC_GROUND_Y);
    mc_render_centered(
        canvas,
        55,
        model->game.mode == McModeTraining && model->game.wave == 3U &&
                (model->game.training_flags & 24U) != 24U ?
            (model->game.training_flags & 8U ? "Buy ammo or radius next" : "Repair a site first") :
            "Back: next wave");
}

static void mc_draw_game_over(Canvas* canvas, const McRenderSnapshot* model) {
    mc_render_heading(
        canvas,
        1,
        model->game.victory   ? "MISSION COMPLETE" :
        model->new_high_score ? "NEW HIGH SCORE" :
                                "GAME OVER");
    mc_render_printf(
        canvas, 13, "Score %lu  W%u", (unsigned long)model->game.score, model->game.wave);
    mc_draw_menu_rows(canvas, model, McMenuGameOver);
}

static void mc_format_duration(char* output, size_t size, uint32_t ticks) {
    const uint32_t seconds = ticks / MC_TICKS_PER_SECOND;
    snprintf(
        output, size, "%lu:%02lu", (unsigned long)(seconds / 60U), (unsigned long)(seconds % 60U));
}

static void mc_draw_stats(
    Canvas* canvas,
    const char* title,
    uint32_t score,
    uint16_t wave,
    McDifficulty difficulty,
    const McRunStats* stats,
    uint8_t cities,
    uint32_t timestamp) {
    char text[40];
    mc_render_heading(canvas, 1, title);
    mc_render_printf(
        canvas,
        15,
        "%.3s %lu W%u",
        mc_render_difficulty_name(difficulty),
        (unsigned long)score,
        wave);
    char duration[16];
    mc_format_duration(duration, sizeof(duration), stats->play_ticks);
    if(timestamp > 0U) {
        DateTime date;
        datetime_timestamp_to_datetime(timestamp, &date);
        snprintf(
            text,
            sizeof(text),
            "%02u/%02u/%02u  Time %s",
            date.month,
            date.day,
            date.year % 100U,
            duration);
    } else {
        snprintf(text, sizeof(text), "Time %s  Cities %u", duration, cities);
    }
    mc_render_centered(canvas, 25, text);
    const uint32_t hit_rate = mc_accuracy_x100(stats->successful_shots, stats->shots_fired) / 100U;
    mc_render_printf(
        canvas,
        35,
        "Shots %lu  Hit %lu%%",
        (unsigned long)stats->shots_fired,
        (unsigned long)hit_rate);
    mc_render_printf(
        canvas,
        45,
        "Kills %lu  Chain %u",
        (unsigned long)stats->enemies_destroyed,
        stats->max_chain_depth);
    mc_render_printf(
        canvas, 55, "Lost %u Fixed %u Cities %u", stats->sites_lost, stats->sites_repaired, cities);
}

static void mc_draw_scores(Canvas* canvas, const McRenderSnapshot* model) {
    char text[36];
    snprintf(text, sizeof(text), "%s SCORES", mc_render_difficulty_name(model->score_difficulty));
    mc_render_heading(canvas, 1, text);
    const McScoreBoard* board = mc_score_board(&model->scores, model->score_difficulty);
    for(uint8_t i = 0U; i < MC_SCORE_CAPACITY; i++) {
        const McScoreEntry* entry = &board->entries[i];
        snprintf(
            text,
            sizeof(text),
            "%u  %08lu  W%03u",
            i + 1U,
            (unsigned long)entry->score,
            entry->wave);
        mc_render_row(canvas, 18 + i * 9, text, model->score_index == i);
    }
    mc_render_centered(canvas, 56, "< Board >  OK Details");
}

static void mc_draw_score_details(Canvas* canvas, const McRenderSnapshot* model) {
    const McScoreBoard* board = mc_score_board(&model->scores, model->score_difficulty);
    const McScoreEntry* entry = &board->entries[model->score_index];
    char title[20];
    snprintf(title, sizeof(title), "SCORE #%u", model->score_index + 1U);
    const McRunStats stats = {
        .shots_fired = entry->stats.shots_fired,
        .successful_shots = entry->stats.successful_shots,
        .enemies_destroyed = entry->stats.enemies_destroyed,
        .play_ticks = entry->stats.play_ticks,
        .sites_lost = entry->stats.sites_lost,
        .sites_repaired = entry->stats.sites_repaired,
        .max_chain_depth = entry->stats.max_chain_depth,
    };
    mc_draw_stats(
        canvas,
        title,
        entry->score,
        entry->wave,
        entry->difficulty,
        &stats,
        entry->cities_remaining,
        entry->timestamp);
}

static void mc_draw_records(Canvas* canvas, const McRenderSnapshot* model) {
    static const struct {
        uint16_t field;
        uint8_t label, format;
    } rows[] = {
        {MC_INFO_FIELD(McProfile, best_score), 0, 0},
        {MC_INFO_FIELD(McProfile, best_wave), 6, 0},
        {MC_INFO_FIELD(McProfile, best_survival_ticks), 11, 1},
        {MC_INFO_FIELD(McProfile, best_accuracy_x100), 20, 2},
        {MC_INFO_FIELD(McProfile, best_chain), 29, 0},
        {MC_INFO_FIELD(McProfile, best_perfect_streak), 35, 0},
    };
    static const char labels[] = "Score\0Wave\0Survival\0Accuracy\0Chain\0Perfect streak";
    char text[36];
    mc_render_heading(canvas, 1, "PERSONAL RECORDS");
    const uint8_t first = mc_list_first(model->record_index, McInfoLayouts[McInfoRecords].rows);
    for(uint8_t i = first; i < first + McInfoLayouts[McInfoRecords].rows; i++) {
        const char* label = labels + rows[i].label;
        const uint32_t value = mc_info_value(&model->profile, rows[i].field);
        if(rows[i].format == 1) {
            char duration[16];
            mc_format_duration(duration, sizeof(duration), value);
            snprintf(text, sizeof(text), "%s: %s", label, duration);
        } else if(rows[i].format == 2)
            snprintf(
                text,
                sizeof(text),
                "%s: %u.%02u%%",
                label,
                (unsigned)(value / 100U),
                (unsigned)(value % 100U));
        else
            snprintf(text, sizeof(text), "%s: %lu", label, (unsigned long)value);
        mc_render_row(
            canvas,
            McInfoLayouts[McInfoRecords].y + (i - first) * McInfoLayouts[McInfoRecords].step,
            text,
            model->record_index == i);
    }
    mc_render_centered(canvas, 57, "<> Modes  OK Medals");
}

static void mc_draw_medals(Canvas* canvas, const McRenderSnapshot* model) {
    mc_render_heading(canvas, 1, "MEDALS");
    const uint8_t visible = 5U;
    const uint8_t first = mc_list_first(model->medal_index, visible);
    for(uint8_t row = 0U; row < visible; row++) {
        const uint8_t medal = first + row;
        char text[32];
        const bool earned = (model->profile.earned_medals & (1U << medal)) != 0U;
        snprintf(text, sizeof(text), "%c %s", earned ? '*' : '-', mc_medal_name(medal));
        mc_render_row(canvas, 18 + row * 9, text, model->medal_index == medal);
    }
    mc_draw_scroll_arrows(canvas, first, visible, McMedalCount);
}

static void mc_draw_medal_details(Canvas* canvas, const McRenderSnapshot* model) {
    const McMedal medal = model->medal_index;
    const bool earned = (model->profile.earned_medals & (1U << medal)) != 0U;
    mc_render_heading(canvas, 3, mc_medal_name(medal));
    mc_render_help(canvas, model, -1, 23, 0);
    uint16_t value, target;
    mc_medal_progress(&model->profile, &model->game, medal, &value, &target);
    mc_render_printf(canvas, 38, "%s %u/%u", earned ? "Earned" : "Progress", value, target);
    mc_render_centered(
        canvas,
        55,
        model->profile.pinned_medal == medal ? "OK Unpin  Back Return" :
                                               "OK Pin goal  Back Return");
}

static void mc_draw_settings(Canvas* canvas, const McRenderSnapshot* model) {
    char text[32];
    mc_render_heading(canvas, 1, mc_settings_group_name(model->settings_group));
    const uint8_t count = mc_settings_group_count(model->settings_group);
    uint8_t selected = 0U;
    while(selected < count &&
          mc_settings_group_item(model->settings_group, selected) != model->menu_index)
        selected++;
    // Scroll by group row, not by the global setting identifier
    const uint8_t first = selected >= MC_SETTINGS_VISIBLE_ITEMS && selected < count ?
                              selected - MC_SETTINGS_VISIBLE_ITEMS + 1U :
                              0U;
    for(uint8_t row = 0U; row < MC_SETTINGS_VISIBLE_ITEMS && first + row < count; row++) {
        const uint8_t item = mc_settings_group_item(model->settings_group, first + row);
        const char* label = mc_setting_label(item);
        const char* value = mc_setting_value(&model->common, item);
        if(value) snprintf(text, sizeof(text), "%s: %s", label, value);
        mc_render_row(canvas, 18 + row * 9, value ? text : label, model->menu_index == item);
    }
    mc_draw_scroll_arrows(canvas, first, MC_SETTINGS_VISIBLE_ITEMS, count);
}

static __attribute__((noinline)) void
    mc_draw_setup(Canvas* canvas, const McRenderSnapshot* model) {
    char text[32];
    mc_render_heading(canvas, 1, "NEW GAME");
    const uint8_t first = mc_list_first(model->menu_index, 4);
    for(uint8_t row = 0U; row < 4U; row++) {
        const uint8_t i = first + row;
        if(i == 0U)
            snprintf(text, sizeof(text), "Mode: %s", mc_mode_name(model->setup_mode));
        else if(i == 1U) {
            const McDifficulty difficulty =
                mc_mode_difficulty(model->setup_mode, model->settings.difficulty);
            snprintf(
                text,
                sizeof(text),
                "%s: %s",
                (mc_mode_def(model->setup_mode)->difficulty < McDifficultyCount) ? "Fixed" :
                                                                                   "Difficulty",
                mc_render_difficulty_name(difficulty));
        } else if(i == 2U)
            snprintf(
                text,
                sizeof(text),
                model->setup_mode == McModeDaily ? "Seed: Auto %08lX" :
                (mc_mode_def(model->setup_mode)->seed_policy == McSeedTraining ||
                 mc_mode_def(model->setup_mode)->seed_policy == McSeedPuzzle) ?
                                                   "Scenario: Fixed" :
                                                   "Seed: %08lX",
                (unsigned long)model->setup_seed);
        else if(i == 3U)
            snprintf(text, sizeof(text), "Start run (slot %u)", model->active_slot + 1U);
        else
            snprintf(
                text,
                sizeof(text),
                "%s options",
                model->setup_mode == McModePractice ? "Practice" : "Puzzle");
        mc_render_row(canvas, 19 + row * 10U, text, model->menu_index == i);
    }
    mc_draw_scroll_arrows(
        canvas, first, 4U, (mc_mode_def(model->setup_mode)->options != McOptionsNone) ? 5U : 4U);
    if(model->setup_mode == McModeDaily) {
        const uint32_t best =
            model->profile.daily_seed == model->setup_seed ? model->profile.daily_best : 0U;
        mc_render_printf(canvas, 56, "Today's best: %lu", (unsigned long)best);
    } else
        mc_render_help(canvas, model, -1, 56, 0);
}

static void mc_draw_lesson(Canvas* canvas, const McRenderSnapshot* model) {
    const uint8_t lesson = model->game.wave > 4U ? 3U :
                           model->game.wave      ? model->game.wave - 1U :
                                                   0U;
    mc_render_heading(
        canvas,
        1,
        mc_text_at(
            "1: LEAD TARGETS\0"
            "2: CHAIN REACTIONS\0"
            "3: BATTERY CONTROL\0"
            "4: REPAIR & SUPPLY",
            lesson));
    mc_render_help(canvas, model, -1, 19, 10);
    mc_render_centered(canvas, 55, "OK Practice  Back Pause");
}

static void mc_draw_mode_records(Canvas* canvas, const McRenderSnapshot* model) {
    char text[36];
    mc_render_heading(canvas, 1, "MODE RECORDS");
    const uint8_t first = mc_list_first(model->record_index, 5);
    for(uint8_t i = first; i < first + 5U && i < McModeCount; i++) {
        if(!mc_mode_has(i, McModeCompetitive))
            snprintf(text, sizeof(text), "%.12s: Unranked", mc_mode_name(i));
        else
            snprintf(
                text,
                sizeof(text),
                "%.12s: %lu",
                mc_mode_name(i),
                (unsigned long)model->profile.mode_best[i]);
        mc_render_menu_item(canvas, 2, 18 + (i - first) * 9U, 124, text, model->record_index == i);
    }
    mc_render_centered(canvas, 56, "<> Records  Back");
}

static void mc_draw_about(Canvas* canvas) {
    mc_render_heading(canvas, 1, "Missile CMD");
    mc_render_printf(canvas, 17, "Version %s", MC_APP_VERSION);
    mc_render_centered(canvas, 27, "LTDev LLC");
    mc_render_centered(canvas, 37, "MIT License");
    mc_render_centered(canvas, 57, "Back to return");
}

static void mc_draw_confirmation(Canvas* canvas, McConfirmAction action) {
    static const char labels[] = "CONFIRM?\0This cannot be undone\0"
                                 "NEW GAME?\0Replace saved run\0"
                                 "RESTART RUN?\0Replace current run\0"
                                 "ABANDON RUN?\0Delete saved progress\0"
                                 "SAVE & TITLE?\0Suspend current run\0"
                                 "RESET SETTINGS?\0Restore all defaults\0"
                                 "RESET SCORES?\0Clear all 15 scores\0"
                                 "CLEAR PACE HISTORY?\0Scores and saves stay";
    const char* title = mc_text_at(labels, (action <= McConfirmClearPace ? action : 0) * 2U);
    const char* body = mc_text_at(title, 1);
    mc_render_heading(canvas, 8, title);
    mc_render_centered(canvas, 29, body);
    mc_render_centered(canvas, 49, "OK Confirm  Back Cancel");
}

// Prevent inlining the large dispatcher to limit runtime callback register spills
static __attribute__((noinline)) void
    mc_draw_expansion(Canvas* canvas, const McRenderSnapshot* m) {
    char text[48];
    const char* title = m->screen == McScreenSettingsGroups ? "SETTINGS" :
                        m->screen == McScreenPracticeSetup  ? "MODE OPTIONS" :
                        m->screen == McScreenStorage        ? "SAVE STATUS" :
                        m->screen == McScreenSlots          ? "SAVED RUNS" :
                        m->screen == McScreenDuelSwap       ? "PASS TO PLAYER 2" :
                                                              "TWO-PLAYER RESULTS";
    mc_render_heading(canvas, 1, title);
    switch(m->screen) {
    case McScreenSettingsGroups:
        for(uint8_t i = 0U; i < 4U; i++)
            mc_render_row(canvas, 22 + 10U * i, mc_settings_group_name(i), i == m->menu_index);
        break;
    case McScreenPracticeSetup: {
        const bool puzzle = m->setup_mode == McModePuzzle;
        if(puzzle) {
            snprintf(
                text,
                sizeof(text),
                "%u/9 %s",
                m->setup_options.start_wave + 1U,
                mc_puzzle_name(m->setup_options.start_wave));
            mc_render_row(canvas, 20, text, m->menu_index == 0U);
            mc_render_printf(
                canvas,
                30,
                "%s / %s",
                mc_render_difficulty_name(m->settings.difficulty),
                m->profile.puzzles_completed[m->settings.difficulty] &
                        (1U << m->setup_options.start_wave) ?
                    "Cleared" :
                    "Not cleared");
            mc_render_row(canvas, 49, "View hint", m->menu_index == 1U);
        } else {
            const uint8_t first = m->menu_index >= McInfoLayouts[McInfoPractice].rows ? 1U : 0U;
            for(uint8_t row = 0U; row < McInfoLayouts[McInfoPractice].rows; row++) {
                const uint8_t i = first + row;
                static const struct {
                    uint16_t field;
                    uint8_t label, values;
                } rows[] = {
                    {MC_INFO_FIELD(McUiCommon, setup_options.enemy), 0, 0},
                    {MC_INFO_FIELD(McUiCommon, setup_options.unlimited), 8, 5},
                    {MC_INFO_FIELD(McUiCommon, setup_options.slow), 23, 7},
                    {MC_INFO_FIELD(McUiCommon, settings.practice_aids), 29, 5},
                };
                static const char labels[] = "Enemies\0Unlimited ammo\0Speed\0Aiming aids";
                static const char values[] =
                    "Mixed\0Standard\0Fast\0Splitter\0Evasive\0Off\0On\0Normal\0Half";
                if(i == 0U)
                    snprintf(
                        text, sizeof(text), "Starting wave: %u", m->setup_options.start_wave + 1U);
                else {
                    const uint8_t item = i - 1U;
                    snprintf(
                        text,
                        sizeof(text),
                        "%s: %s",
                        labels + rows[item].label,
                        mc_text_at(
                            values,
                            rows[item].values + mc_info_value(&m->common, rows[item].field)));
                }
                mc_render_row(
                    canvas,
                    McInfoLayouts[McInfoPractice].y + row * McInfoLayouts[McInfoPractice].step,
                    text,
                    i == m->menu_index);
            }
            mc_draw_scroll_arrows(canvas, first, McInfoLayouts[McInfoPractice].rows, 5U);
        }
        mc_render_centered(canvas, 56, "<> Change  Back Done");
        break;
    }
    case McScreenSlots:
        for(uint8_t i = 0U; i < MC_SAVE_SLOTS; i++) {
            const McSaveInfo* slot = &m->slots[i];
            if(slot->status == McStorageOk)
                snprintf(
                    text, sizeof(text), "%u %s W%u", i + 1U, mc_mode_name(slot->mode), slot->wave);
            else
                snprintf(
                    text,
                    sizeof(text),
                    "%u %s",
                    i + 1U,
                    slot->status == McStorageBusy    ? "Loading..." :
                    slot->status == McStorageMissing ? "Empty" :
                                                       "Unreadable");
            mc_render_row(canvas, 20 + i * 10U, text, i == m->menu_index);
        }
        if(m->slots[m->menu_index].status == McStorageOk)
            snprintf(
                text,
                sizeof(text),
                "%s Score %lu",
                mc_render_difficulty_name(m->slots[m->menu_index].difficulty),
                (unsigned long)m->slots[m->menu_index].score);
        else
            snprintf(text, sizeof(text), "Select slot for next run");
        mc_render_centered(canvas, 46, text);
        if(m->slots[m->menu_index].status == McStorageOk && m->slots[m->menu_index].timestamp) {
            DateTime date;
            datetime_timestamp_to_datetime(m->slots[m->menu_index].timestamp, &date);
            snprintf(
                text,
                sizeof(text),
                "%02u/%02u/%02u  OK Select",
                date.month,
                date.day,
                date.year % 100U);
        } else
            snprintf(text, sizeof(text), "OK Select  Back Title");
        mc_render_centered(canvas, 56, text);
        break;
    case McScreenStorage:
        mc_render_centered(canvas, 19, m->storage_item ? m->storage_item : "Storage");
        mc_render_centered(
            canvas,
            31,
            m->storage_result == McStorageBusy    ? "Working..." :
            m->storage_result == McStorageOk      ? "Saved successfully" :
            m->storage_result == McStorageMissing ? "Nothing saved here" :
            m->storage_result == McStorageInvalid ? "Invalid or damaged data" :
                                                    "Read/write failed");
        mc_render_centered(
            canvas,
            43,
            m->storage_result == McStorageBusy ? "Please wait" :
            m->storage_pending                 ? "Check SD card; retry below" :
                                                 "No pending error");
        mc_render_centered(canvas, 55, "OK Retry  Back Return");
        break;
    case McScreenDuelSwap:
        mc_render_printf(canvas, 22, "Player 1: %lu", (unsigned long)m->duel[0].score);
        mc_render_centered(canvas, 35, "Same seed and rules");
        mc_render_centered(canvas, 55, "OK Start P2  Back End");
        break;
    case McScreenDuelResults:
        mc_render_centered(
            canvas,
            15,
            m->duel[0].score == m->duel[1].score ? "DRAW" :
            m->duel[0].score > m->duel[1].score  ? "PLAYER 1 WINS" :
                                                   "PLAYER 2 WINS");
        for(uint8_t i = 0U; i < 2U; i++) {
            snprintf(
                text,
                sizeof(text),
                "P%u %lu  %lu s  %u%%",
                i + 1U,
                (unsigned long)m->duel[i].score,
                (unsigned long)(m->duel[i].ticks / 30U),
                mc_accuracy_x100(m->duel[i].hits, m->duel[i].shots) / 100U);
            mc_render_centered(canvas, 29 + 12U * i, text);
        }
        mc_render_centered(canvas, 55, "OK / Back Title");
        break;
    default:
        break;
    }
}

static void mc_draw_cleanup(Canvas* canvas, const McRenderSnapshot* model) {
    mc_render_heading(canvas, 1, "VERSION DATA");
    if(model->cleanup_state == McCleanupScanning) {
        mc_render_centered(canvas, 27, "Checking data folders...");
        return;
    }
    if(model->cleanup_state == McCleanupPurging) {
        mc_render_centered(
            canvas,
            23,
            model->cleanup_migrating ? "Pruning older folders" : "Deleting approved folders");
        mc_render_printf(
            canvas, 37, "%lu folders remaining", (unsigned long)model->cleanup_remaining);
        return;
    }
    if(model->cleanup_state == McCleanupMigrating || model->cleanup_state == McCleanupValidating) {
        mc_render_centered(
            canvas,
            23,
            model->cleanup_state == McCleanupMigrating ? "Copying and verifying..." :
                                                         "Checking settings / saves");
        mc_render_centered(canvas, 37, "Old data kept until ready");
        return;
    }
    const bool migrate = model->cleanup_can_migrate && model->cleanup_state == McCleanupPrompt;
    if(model->cleanup_state == McCleanupFailed) {
        mc_render_centered(
            canvas,
            17,
            model->cleanup_migrating ? "Migration incomplete" :
            model->cleanup_approved  ? "Cleanup incomplete" :
                                       "Folder scan failed");
        mc_render_centered(
            canvas,
            29,
            model->storage_result == McStorageInvalid ? "Unreadable settings / saves" :
            model->cleanup_limited                    ? "Too many folders; choose Keep" :
                                                        "Check SD card; retry or keep");
        if(model->cleanup_migrating && model->storage_result == McStorageInvalid)
            mc_render_centered(canvas, 37, "Older folders kept");
    } else if(migrate && model->menu_index == 1U) {
        mc_render_centered(canvas, 13, "From newest older data:");
        mc_render_printf(canvas, 21, "%.20s", model->cleanup_source);
        if(strlen(model->cleanup_source) > 20U)
            mc_render_centered(canvas, 29, model->cleanup_source + 20U);
        mc_render_centered(canvas, 37, "Copy missing; prune old");
    } else {
        mc_render_printf(
            canvas,
            13,
            "Folder %lu/%lu  Part %u",
            (unsigned long)(model->cleanup_index + 1U),
            (unsigned long)model->cleanup_count,
            model->cleanup_offset / 40U + 1U);
        mc_render_printf(canvas, 21, "%.20s", model->cleanup_name);
        if(strlen(model->cleanup_name) > 20U)
            mc_render_centered(canvas, 29, model->cleanup_name + 20U);
        mc_render_centered(
            canvas,
            37,
            model->menu_index ? "Permanently delete all?" : "Keep all version folders");
    }
    mc_render_menu_item(
        canvas, migrate ? 1 : 10, 54, migrate ? 34 : 50, "Keep", model->menu_index == 0U);
    if(migrate) mc_render_menu_item(canvas, 37, 54, 50, "Migrate", model->menu_index == 1U);
    mc_render_menu_item(
        canvas,
        migrate ? 89 : 68,
        54,
        migrate ? 38 : 50,
        model->cleanup_state == McCleanupFailed ? "Retry" : "Purge",
        model->menu_index == (migrate ? 2U : 1U));
    mc_render_centered(canvas, 57, "Up/Down View  Back Keep");
}

void mc_render_screen(Canvas* canvas, const McRenderSnapshot* model) {
    if(model->screen == McScreenExiting) {
        if(model->exit_discard) {
            mc_render_heading(canvas, 1, "EXITING");
            mc_render_centered(canvas, 29, "Finishing storage...");
            return;
        }
        mc_render_heading(canvas, 1, "SAVING BEFORE EXIT");
        mc_render_centered(
            canvas,
            15,
            model->storage_result == McStorageIoError ? "Save failed" :
            model->exit_slow                          ? "Still saving..." :
                                                        "Saving changes...");
        mc_render_row(canvas, 32, "Retry", model->menu_index == 0);
        mc_render_row(canvas, 43, "Return to title", model->menu_index == 1);
        mc_render_row(canvas, 54, "Exit without saving", model->menu_index == 2);
        return;
    }
    if(model->screen == McScreenConfirm && model->confirm_action == McConfirmExitUnsaved) {
        mc_render_heading(canvas, 1, "DISCARD CHANGES?");
        mc_render_centered(canvas, 24, "Latest changes may be lost");
        mc_render_centered(canvas, 40, "OK Discard and exit");
        mc_render_centered(canvas, 54, "Back Keep saving");
        return;
    }
    if(model->screen == McScreenPracticeResult) {
        mc_render_heading(canvas, 2, model->game.victory ? "WAVE PRACTICED" : "TRY WAVE AGAIN");
        mc_render_row(canvas, 30, "Retry wave", model->menu_index == 0);
        mc_render_row(canvas, 42, "Return to title", model->menu_index == 1);
        return;
    }
    switch(model->screen) {
    case McScreenCleanup:
        mc_draw_cleanup(canvas, model);
        break;
    case McScreenPracticeSetup:
    case McScreenSettingsGroups:
    case McScreenStorage:
    case McScreenSlots:
    case McScreenDuelSwap:
    case McScreenDuelResults:
        mc_draw_expansion(canvas, model);
        break;
    case McScreenRunSetup:
        mc_draw_setup(canvas, model);
        break;
    case McScreenLesson:
        mc_draw_lesson(canvas, model);
        break;
    case McScreenModeRecords:
        mc_draw_mode_records(canvas, model);
        break;
    case McScreenLoading:
        mc_render_heading(canvas, 18, "LOADING");
        mc_render_centered(canvas, 35, model->storage_item);
        break;
    case McScreenWaveEditor:
    case McScreenSeed: {
        const bool wave = model->screen == McScreenWaveEditor;
        char seed[9];
        snprintf(
            seed,
            sizeof(seed),
            wave ? "%05lu" : "%08lX",
            (unsigned long)(wave ? model->edit_wave : model->setup_seed));
        mc_render_heading(canvas, 1, wave ? "STARTING WAVE" : "SHARED SEED");
        for(uint8_t i = 0U; i < (wave ? 5U : 8U); i++) {
            char digit[] = {seed[i], '\0'};
            mc_render_menu_item(
                canvas, (wave ? 41 : 27) + 9U * i, 30, 9, digit, model->seed_digit == i);
        }
        mc_render_centered(canvas, 40, "<> Digit  Up/Down Change");
        mc_render_centered(canvas, 55, wave ? "OK Apply  Back Cancel" : "OK / Back Done");
        break;
    }
    case McScreenTitle:
        mc_draw_menu(canvas, model, McMenuTitle, "Missile CMD");
        break;
    case McScreenControlCard:
        mc_draw_control_card(canvas);
        break;
    case McScreenHudGuide:
        mc_draw_hud_guide(canvas, model);
        break;
    case McScreenPaused:
        mc_draw_menu(canvas, model, McMenuPause, "PAUSED");
        {
            char seed[32];
            snprintf(
                seed,
                sizeof(seed),
                "%08lX %s",
                (unsigned long)model->game.seed,
                mc_mode_name(model->game.mode));
            if(model->profile.pinned_medal < McMedalCount) {
                uint16_t value, target;
                mc_medal_progress(
                    &model->profile, &model->game, model->profile.pinned_medal, &value, &target);
                snprintf(
                    seed,
                    sizeof(seed),
                    "%.16s %u/%u",
                    mc_medal_name(model->profile.pinned_medal),
                    value,
                    target);
            }
            mc_render_centered(canvas, 56, seed);
        }
        break;
    case McScreenWaveResult:
        mc_draw_wave_result(canvas, model);
        break;
    case McScreenRepair:
        mc_draw_repair(canvas, model);
        break;
    case McScreenGameOver:
        mc_draw_game_over(canvas, model);
        break;
    case McScreenPuzzleHint: {
        const uint16_t puzzle = model->detail_return_screen == McScreenPaused ?
                                    model->game.options.start_wave :
                                    model->setup_options.start_wave;
        mc_render_heading(canvas, 1, "PUZZLE HINT");
        mc_render_centered(canvas, 16, mc_puzzle_name(puzzle));
        mc_render_help(canvas, model, -1, 30, 10);
        mc_render_centered(canvas, 55, "OK / Back Return");
        break;
    }
    case McScreenRunStats:
        if(model->stats_page) {
            mc_draw_stats(
                canvas,
                "RUN STATS  2/2",
                model->game.score,
                model->game.wave,
                model->game.difficulty,
                &model->game.stats,
                mc_game_alive_cities(&model->game),
                0U);
        } else {
            char text[40];
            const McGame* game = &model->game;
            mc_render_heading(canvas, 1, "RUN SUMMARY  1/2");
            mc_render_printf(
                canvas,
                16,
                "No hit %lu  Ammo %u",
                (unsigned long)(game->stats.shots_fired - game->stats.successful_shots),
                mc_game_total_ammo(game));
            if(game->last_lost_site == UINT8_MAX)
                snprintf(text, sizeof(text), "Last loss: none");
            else if(game->last_lost_site % 3U == 0U)
                snprintf(
                    text,
                    sizeof(text),
                    "Last: %s battery",
                    mc_text_at("left\0center\0right", game->last_lost_site / 3U));
            else
                snprintf(
                    text,
                    sizeof(text),
                    "Last: city %u",
                    game->last_lost_site - game->last_lost_site / 3U);
            mc_render_centered(canvas, 27, text);
            mc_render_help(canvas, model, -1, 41, 0);
            mc_render_centered(canvas, 55, "<> Stats  OK/Back Return");
        }
        break;
    case McScreenHighScores:
        mc_draw_scores(canvas, model);
        break;
    case McScreenScoreDetails:
        mc_draw_score_details(canvas, model);
        break;
    case McScreenRecords:
        mc_draw_records(canvas, model);
        break;
    case McScreenMedals:
        mc_draw_medals(canvas, model);
        break;
    case McScreenMedalDetails:
        mc_draw_medal_details(canvas, model);
        break;
    case McScreenSettings:
        mc_draw_settings(canvas, model);
        break;
    case McScreenAbout:
        mc_draw_about(canvas);
        break;
    case McScreenConfirm:
        mc_draw_confirmation(canvas, model->confirm_action);
        if(model->confirm_action == McConfirmClearPace) {
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_box(canvas, 0, 43, 128, 21);
            canvas_set_color(canvas, ColorBlack);
            mc_render_menu_item(canvas, 4, 54, 58, "Cancel", !model->menu_index);
            mc_render_menu_item(canvas, 68, 54, 56, "Clear", model->menu_index != 0);
        }
        break;
    case McScreenPlaying:
        break;
    }
}
