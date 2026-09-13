#include "help_ui.h"
#include "app.h"

#include "app_internal.h"
#include "balance.h"
#include "controls.h"
#include "render.h"
#include "runtime.h"

#include <furi_hal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MC_INPUT_QUEUE_CAPACITY 32U
#define MC_SAVE_DEBOUNCE_MS     750U
#define MC_STORAGE_NOTICE_MS    5000U

static bool mc_app_exit_required(const McApp* app);
static bool mc_app_version_data_ready(const McApp* app);

// Signed subtraction handles tick-counter wrap for nearby deadlines
static bool mc_tick_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t mc_tick_until(uint32_t now, uint32_t deadline) {
    return mc_tick_reached(now, deadline) ? 0U : deadline - now;
}

// Compare encoded fields so struct padding cannot create false changes
static bool mc_settings_equal(const McSettings* left, const McSettings* right) {
    uint8_t a[MC_SETTINGS_ENCODED_SIZE], b[MC_SETTINGS_ENCODED_SIZE];
    return mc_settings_encode(left, a, sizeof(a)) && mc_settings_encode(right, b, sizeof(b)) &&
           !memcmp(a, b, sizeof(a));
}

static void mc_app_show_storage_notice(McApp* app, McStorageResult result, uint32_t now) {
    if(result == McStorageInvalid || result == McStorageIoError || app->io_recovered) {
        app->ui.storage_notice_visible = true;
        app->ui.storage_recovered = result == McStorageOk && app->io_recovered;
        app->io_recovered = false;
        app->notice_expiry_tick = now + furi_ms_to_ticks(MC_STORAGE_NOTICE_MS);
    }
}

static void mc_app_set_high_score(McApp* app, McDifficulty difficulty) {
    if(app->ui.wave_practice) {
        app->ui.high_score = 0;
        return;
    }
    const McScoreBoard* board = mc_score_board(&app->ui.scores, difficulty);
    const McGame* game = &app->ui.game;
    app->ui.high_score = board ? board->entries[0].score : 0U;
    if(game->phase != McGamePhaseIdle && !mc_game_ranked(game))
        app->ui.high_score =
            game->mode == McModeDaily ?
                (app->ui.profile.daily_seed == game->seed ? app->ui.profile.daily_best : 0U) :
                (mc_game_competitive(game) ? app->ui.profile.mode_best[game->mode] : 0U);
}

void mc_app_refresh_high_score(McApp* app) {
    mc_app_set_high_score(app, app->ui.settings.difficulty);
}

// Cache HUD text and aim estimates by their inputs so drawing consumes prepared values
void mc_app_refresh_render_cache(McApp* app) {
    const McGame* game = &app->ui.game;
    const int8_t selected = mc_game_selected_battery(game);
    uint16_t aim = 0U;
    if(!app->ui.hud_valid) app->ui.aim_valid = false;
    // Shot availability can change while the cached flight distance stays valid
    if((game->mode == McModePractice || app->ui.wave_practice) && app->ui.settings.practice_aids &&
       game->phase == McGamePhasePlaying && selected >= 0 &&
       mc_game_site_ammo(game, (uint8_t)selected) &&
       game->interceptor_count < MC_INTERCEPTOR_CAPACITY) {
        // Shots target integer pixels, so subpixel motion does not invalidate this cache
        const uint8_t x = mc_game_cursor_x(game), y = mc_game_cursor_y(game);
        const uint8_t speed = game->supplies & 1U;
        if(!app->ui.aim_valid || app->ui.aim_x != x || app->ui.aim_y != y ||
           app->ui.aim_battery != selected || app->ui.aim_speed != speed) {
            app->ui.aim_duration = mc_game_aim_ticks(game);
            app->ui.aim_x = x;
            app->ui.aim_y = y;
            app->ui.aim_battery = selected;
            app->ui.aim_speed = speed;
            app->ui.aim_valid = true;
        }
        aim = app->ui.aim_duration * (game->options.slow ? 2U : 1U);
    }
    if(!app->ui.hud_valid || aim != app->ui.aim_ticks) {
        const uint16_t tenths = (aim + 2U) / 3U;
        snprintf(app->ui.aim_text, sizeof(app->ui.aim_text), "%u.%us", tenths / 10U, tenths % 10U);
        app->ui.aim_ticks = aim;
    }
    app->ui.warning_sites = app->ui.settings.impact_warnings ? mc_game_impact_warnings(game) : 0U;
    if(game->mode == McModeSprint) {
        const uint16_t seconds = game->stats.play_ticks >= MC_SPRINT_TICKS ?
                                     0U :
                                     (MC_SPRINT_TICKS - game->stats.play_ticks + 29U) / 30U;
        if(!app->ui.hud_valid || seconds != app->ui.cached_seconds) {
            snprintf(
                app->ui.timer_text,
                sizeof(app->ui.timer_text),
                "%u:%02u",
                seconds / 60U,
                seconds % 60U);
            app->ui.cached_seconds = seconds;
        }
    }
    const bool ammo_changed = !app->ui.hud_valid ||
                              app->ui.cached_simple != app->ui.settings.simple_hud ||
                              memcmp(app->ui.cached_ammo, game->battery_ammo, 3U);
    const int8_t previous_battery = app->ui.cached_selected_battery;
    app->ui.cached_selected_battery = selected;
    char prefix = 'A';
    if(game->battery_selection == McBatteryLeft) prefix = 'L';
    if(game->battery_selection == McBatteryCenter) prefix = 'C';
    if(game->battery_selection == McBatteryRight) prefix = 'R';
    uint8_t ammo = 0U;
    bool has_ammo_display = app->ui.cached_selected_battery >= 0;
    if(has_ammo_display) {
        ammo = mc_game_site_ammo(game, (uint8_t)app->ui.cached_selected_battery);
    }
    const bool changed =
        !app->ui.hud_valid || previous_battery != app->ui.cached_selected_battery ||
        app->ui.cached_score != game->score || app->ui.cached_high_score != app->ui.high_score ||
        app->ui.cached_wave != game->wave || app->ui.cached_selection != game->battery_selection ||
        memcmp(app->ui.cached_ammo, game->battery_ammo, 3U) ||
        app->ui.cached_threats != game->enemy_count + game->pending_remaining ||
        app->ui.cached_simple != app->ui.settings.simple_hud;
    if(!changed) return;
    app->ui.hud_valid = true;
    app->ui.cached_score = game->score;
    app->ui.cached_high_score = app->ui.high_score;
    app->ui.cached_wave = game->wave;
    app->ui.cached_selection = game->battery_selection;
    memcpy(app->ui.cached_ammo, game->battery_ammo, 3U);
    app->ui.cached_threats = game->enemy_count + game->pending_remaining;
    app->ui.cached_simple = app->ui.settings.simple_hud;
    if(app->ui.settings.simple_hud) {
        snprintf(
            app->ui.hud_text,
            sizeof(app->ui.hud_text),
            "W%u %lu",
            game->wave,
            (unsigned long)game->score);
    } else {
        const bool wide_wave = game->wave > 99;
        snprintf(
            app->ui.hud_text,
            sizeof(app->ui.hud_text),
            "W%02u%s %06lu H%05lu %c%02u",
            wide_wave ? 99 : game->wave,
            wide_wave ? "+" : "",
            (unsigned long)(game->score > 999999 ? 999999 : game->score),
            (unsigned long)(app->ui.high_score > 99999 ? 99999 : app->ui.high_score),
            prefix,
            ammo);
        if(game->score > 999999) app->ui.hud_text[9 + wide_wave] = '+';
        if(app->ui.high_score > 99999) app->ui.hud_text[16 + wide_wave] = '+';
        if(!has_ammo_display) memcpy(app->ui.hud_text + 18 + wide_wave, "A--", 3);
    }
    if(app->ui.settings.simple_hud) {
        if(ammo_changed)
            snprintf(
                app->ui.footer_text,
                sizeof(app->ui.footer_text),
                "L%u  C%u  R%u",
                game->battery_ammo[0],
                game->battery_ammo[1],
                game->battery_ammo[2]);
    } else
        snprintf(
            app->ui.footer_text,
            sizeof(app->ui.footer_text),
            "T%u L%u C%u R%u%s",
            game->enemy_count + game->pending_remaining,
            game->battery_ammo[0],
            game->battery_ammo[1],
            game->battery_ammo[2],
            mc_game_total_ammo(game) <= 3U ? " LOW" : "");
}

void mc_app_settings_changed(McApp* app) {
    app->settings_revision++;
    const unsigned state = atomic_load_explicit(&app->worker.state, memory_order_acquire);
    const bool writing = (state == McIoPending || state == McIoComplete) &&
                         app->worker.job.operation == McIoSaveSettings;
    // Reverting an edit before the deadline should cancel the write
    if(!writing && mc_settings_equal(&app->ui.settings, &app->saved_settings)) {
        app->settings_dirty = false;
        app->pending_io &= (McPendingIo)~McPendingIoSettings;
        return;
    }
    app->settings_dirty = true;
    app->settings_due_tick = furi_get_tick() + furi_ms_to_ticks(MC_SAVE_DEBOUNCE_MS);
}

void mc_app_flush_settings(McApp* app) {
    if(!app->settings_dirty) return;
    app->settings_due_tick = furi_get_tick();
    app->pending_io |= McPendingIoSettings;
}

void mc_app_save_scores(McApp* app) {
    app->scores_dirty = true;
    app->scores_revision++;
    app->pending_io |= McPendingIoScores;
}

void mc_app_queue_run_save(McApp* app, McRunStage stage, bool return_to_title) {
    if(!mc_session_has(app->ui.game.mode, app->ui.wave_practice, McModeSave)) return;
    app->run_revision++;
    app->pending_run_stage = stage;
    app->run_dirty = true;
    app->save_title_on_success = return_to_title;
    app->pending_io |= McPendingIoRunSave;
}

void mc_app_queue_run_delete(McApp* app) {
    if(!mc_session_has(app->ui.game.mode, app->ui.wave_practice, McModeSave)) return;
    app->ui.has_suspended_run = false;
    app->run_dirty = false;
    // Cancel queued saves so they cannot recreate the deleted checkpoint
    app->pending_io &= (McPendingIo)~McPendingIoRunSave;
    app->pending_io |= McPendingIoRunDelete;
}

void mc_app_change_difficulty(McApp* app, bool increment) {
    const uint8_t current = app->ui.settings.difficulty;
    app->ui.settings.difficulty = mc_wrap_step(current, McDifficultyCount, increment);
    mc_app_refresh_high_score(app);
    mc_app_settings_changed(app);
}

void mc_app_open_run_setup(McApp* app) {
    app->ui.remember_setup = true;
    app->ui.override_difficulty = false;
    app->ui.screen = McScreenRunSetup;
    app->ui.menu_index = 0U;
    app->ui.retry_seed = false;
    app->ui.setup_seed = furi_hal_random_get();
    if(app->ui.setup_seed == 0U) app->ui.setup_seed = 1U;
}

// Finish required writes before changing the run or slot they belong to
static bool mc_app_wait_for_required_io(McApp* app) {
    const unsigned io_state = atomic_load_explicit(&app->worker.state, memory_order_acquire);
    const bool required_active =
        io_state != McIoIdle && io_state != McIoStopped &&
        ((app->worker.job.family &
          (McPendingIoRunLoad | McPendingIoRunSave | McPendingIoRunDelete | McPendingIoPaceLoad |
           McPendingIoPaceSave)) ||
         (app->worker.job.operation == McIoHistory && app->worker.job.data.cleanup.action));
    if(!required_active && !app->clear_history && !app->run_dirty &&
       !((app->pending_io | app->failed_io) &
         (McPendingIoRunLoad | McPendingIoRunSave | McPendingIoRunDelete | McPendingIoPaceSave)) &&
       !app->history_required)
        return false;
    app->ui.detail_return_screen = app->ui.screen;
    app->ui.storage_item = "Previous run pending";
    app->ui.storage_pending = true;
    app->ui.storage_result =
        app->failed_io || (app->run_dirty && app->ui.storage_result == McStorageIoError) ?
            McStorageIoError :
            McStorageBusy;
    app->ui.screen = McScreenStorage;
    return true;
}
void mc_app_start_new_run(McApp* app, bool allow_control_card) {
    if(mc_app_wait_for_required_io(app)) return;
    app->run_identity++;
    app->ui.workshop_reason = McWorkshopOk;
    mc_feedback_stop(&app->feedback);
    mc_app_flush_settings(app);
    // Do not publish writes from the previous run under the new identity
    const McPendingIo previous = McPendingIoRunDelete | McPendingIoRunSave | McPendingIoPaceSave;
    app->pending_io &= (McPendingIo)~previous;
    app->failed_io &= (McPendingIo)~previous;
    app->run_dirty = false;
    app->held_directions = 0U;
    app->cursor_direction_x = 0;
    app->cursor_direction_y = 0;
    app->cursor_hold_ticks = 0U;
    app->ui.battery_overlay = false;
    const uint32_t seed = mc_mode_seed(
        app->ui.setup_mode, app->ui.setup_seed, furi_hal_rtc_get_timestamp(), app->ui.retry_seed);
    app->ui.wave_practice = false;
    mc_game_start_config(
        &app->ui.game,
        app->ui.retry_seed          ? app->ui.game.difficulty :
        app->ui.override_difficulty ? app->ui.setup_difficulty :
                                      app->ui.settings.difficulty,
        seed,
        app->ui.setup_mode,
        &app->ui.setup_options);
    mc_app_capture_wave(app);
    if(app->ui.remember_setup && !app->ui.retry_seed && !app->ui.override_difficulty) {
        McSettings* settings = &app->ui.settings;
        settings->last_setup_valid = true;
        settings->last_mode = app->ui.game.mode;
        settings->last_difficulty = app->ui.game.difficulty;
        settings->last_seed = app->ui.game.seed;
        settings->last_options = app->ui.game.options;
        mc_app_settings_changed(app);
        mc_app_flush_settings(app);
    }
    app->ui.remember_setup = app->ui.override_difficulty = false;
    app->ui.countdown_ticks = app->ui.shot_notice_ticks = 0U;
    app->ui.hud_valid = false;
    app->pending_io |= McPendingIoPaceLoad;
    mc_app_set_high_score(app, app->ui.game.difficulty);
    app->ui.score_recorded = false;
    app->ui.new_high_score = false;
    app->ui.medal_notice_mask = 0U;
    app->ui.menu_index = 0U;
    mc_app_queue_run_save(app, McRunStageActive, false);

    if(app->ui.game.mode == McModeTraining) {
        app->ui.screen = McScreenLesson;
    } else if(allow_control_card && app->ui.settings.show_control_card) {
        app->ui.screen = McScreenControlCard;
    } else {
        mc_app_continue(app);
    }
    app->pending_changes |= McGameChangeAll;
    mc_app_refresh_render_cache(app);
}

void mc_app_resume_run(McApp* app) {
    if(!app->ui.has_suspended_run || mc_app_wait_for_required_io(app)) return;
    mc_app_flush_settings(app);
    app->pending_io |= McPendingIoRunLoad;
}

void mc_app_open_settings(McApp* app, McScreen return_screen) {
    app->ui.settings_return_screen = return_screen;
    app->ui.menu_index = 0U;
    app->ui.settings_group = 0U;
    app->ui.screen = McScreenSettingsGroups;
}

void mc_app_open_scores(McApp* app, McScreen return_screen) {
    app->ui.scores_return_screen = return_screen;
    app->ui.score_difficulty = return_screen == McScreenGameOver ? app->ui.game.difficulty :
                                                                   app->ui.settings.difficulty;
    app->ui.score_index = 0U;
    app->ui.screen = McScreenHighScores;
}

void mc_app_confirm(McApp* app, McConfirmAction action, McScreen return_screen) {
    if(action == McConfirmSaveTitle &&
       (!mc_session_has(app->ui.game.mode, app->ui.wave_practice, McModeSave)))
        action = McConfirmAbandonRun;
    if(action == McConfirmClearPace) app->ui.menu_index = 0;
    app->ui.confirm_action = action;
    app->ui.confirm_return_screen = return_screen;
    app->ui.screen = McScreenConfirm;
}

void mc_app_direction_changed(McApp* app, bool guarantee_step) {
    const bool left = (app->held_directions & MC_HELD_LEFT) != 0U;
    const bool right = (app->held_directions & MC_HELD_RIGHT) != 0U;
    const bool up = (app->held_directions & MC_HELD_UP) != 0U;
    const bool down = (app->held_directions & MC_HELD_DOWN) != 0U;
    const int8_t dx = left == right ? 0 : left ? -1 : 1;
    const int8_t dy = up == down ? 0 : up ? -1 : 1;
    if(dx != app->cursor_direction_x || dy != app->cursor_direction_y) {
        app->cursor_direction_x = dx;
        app->cursor_direction_y = dy;
        app->cursor_hold_ticks = 0U;
    }
    // Apply taps immediately so presses between simulation ticks still register
    if(guarantee_step && (dx != 0 || dy != 0)) {
        uint16_t step = (uint16_t)app->ui.settings.tap_pixels * MC_CURSOR_ONE;
        // 181/256 approximates 1/sqrt(2) to normalize diagonal speed
        if(dx != 0 && dy != 0) step = (uint16_t)(((uint32_t)step * 181U) / 256U);
        app->pending_changes |= mc_game_move_cursor_q8(
            &app->ui.game, (int16_t)(dx * (int16_t)step), (int16_t)(dy * (int16_t)step));
    }
}

static void mc_app_record_score(McApp* app) {
    if(app->ui.score_recorded) return;
    if(!mc_game_ranked(&app->ui.game)) {
        app->ui.score_recorded = true;
        return;
    }
    const McScoreBoard* board = mc_score_board(&app->ui.scores, app->ui.game.difficulty);
    const uint32_t previous_high_score = board ? board->entries[0].score : 0U;
    McScoreEntry entry = {
        .score = app->ui.game.score,
        .timestamp = furi_hal_rtc_get_timestamp(),
        .wave = app->ui.game.wave,
        .difficulty = app->ui.game.difficulty,
        .stats =
            {
                .shots_fired = app->ui.game.stats.shots_fired,
                .successful_shots = app->ui.game.stats.successful_shots,
                .enemies_destroyed = app->ui.game.stats.enemies_destroyed,
                .play_ticks = app->ui.game.stats.play_ticks,
                .sites_lost = app->ui.game.stats.sites_lost,
                .sites_repaired = app->ui.game.stats.sites_repaired,
                .max_chain_depth = app->ui.game.stats.max_chain_depth,
            },
        .cities_remaining = mc_game_alive_cities(&app->ui.game),
    };
    if(mc_score_tables_insert(&app->ui.scores, &entry)) {
        app->ui.new_high_score = entry.score > previous_high_score;
        app->scores_dirty = true;
        app->scores_revision++;
        app->pending_io |= McPendingIoScores;
        mc_app_set_high_score(app, app->ui.game.difficulty);
    }
    app->ui.score_recorded = true;
}

static void mc_app_queue_medals(McApp* app, uint16_t medals) {
    if(medals == 0U) return;
    app->ui.medal_notice_mask |= medals;
    mc_app_profile_changed(app);
}

// Translate simulation events into screen transitions, feedback, and queued persistence
void mc_app_dispatch_events(McApp* app, const McGameEventBuffer* events) {
    if(!events) return;
    if(app->ui.wave_practice) {
        for(uint8_t i = 0; i < events->count; i++) {
            if(events->items[i].type == McGameEventWaveCleared ||
               events->items[i].type == McGameEventGameOver) {
                app->ui.game.victory = events->items[i].type == McGameEventWaveCleared;
                app->ui.game.phase = McGamePhaseGameOver;
                app->ui.screen = McScreenPracticeResult;
                app->ui.menu_index = 0;
                mc_app_clear_directions(app);
                break;
            }
        }
        return;
    }
    // Choose the alert first so fatal impacts suppress ordinary flashes
    McLedCue led = events->explosion ? McLedExplosion : McLedNone;
    for(uint8_t i = 0U; i < events->count; i++) {
        const McGameEventType type = events->items[i].type;
        const McLedCue cue = type == McGameEventGameOver    ? McLedGameOver :
                             type == McGameEventSiteLost    ? McLedSiteLost :
                             type == McGameEventWaveCleared ? McLedWaveClear :
                                                              McLedNone;
        if(cue > led) led = cue;
    }
    mc_feedback_led(&app->feedback, app->ui.tick, led);
    // Coalesce simultaneous hits into the strongest chain cue this tick
    uint8_t best_chain = 0U;
    for(uint8_t i = 0U; i < events->count; i++) {
        const McGameEvent* event = &events->items[i];
        switch(event->type) {
        case McGameEventEnemyDestroyed:
            if(event->chain_depth > best_chain) best_chain = event->chain_depth;
            if(mc_game_ranked(&app->ui.game) && event->chain_depth > app->ui.profile.best_chain) {
                app->ui.profile.best_chain = event->chain_depth;
                mc_app_profile_changed(app);
            }
            if(mc_game_ranked(&app->ui.game) && event->chain_depth >= 5U &&
               (app->ui.profile.earned_medals & (1U << McMedalChainReaction)) == 0U) {
                app->ui.profile.earned_medals |= 1U << McMedalChainReaction;
                mc_app_queue_medals(app, 1U << McMedalChainReaction);
            }
            break;
        case McGameEventSiteLost:
            mc_feedback_site_lost(&app->feedback);
            break;
        case McGameEventWaveCleared:
            mc_app_clear_directions(app);
            app->ui.screen = McScreenWaveResult;
            app->ui.stats_page = 0U;
            const uint16_t wave = app->ui.game.wave;
            app->ui.pace_available = wave > 0U && wave <= MC_PACE_WAVES &&
                                     app->pace.waves[wave - 1U] != 0U;
            if(app->ui.pace_available) {
                const int64_t delta = (int64_t)app->ui.game.score - app->pace.waves[wave - 1U];
                app->ui.pace_delta = delta > INT32_MAX ? INT32_MAX :
                                     delta < INT32_MIN ? INT32_MIN :
                                                         (int32_t)delta;
            }
            app->ui.menu_index = 0U;
            mc_feedback_wave_clear(&app->feedback);
            mc_app_queue_medals(app, mc_profile_update_wave(&app->ui.profile, &app->ui.game));
            mc_app_profile_changed(app);
            mc_app_queue_run_save(app, McRunStageWaveResult, false);
            break;
        case McGameEventGameOver:
            mc_app_clear_directions(app);
            app->ui.screen = McScreenGameOver;
            app->ui.menu_index = 0U;
            if(app->ui.game.mode == McModeDuel) {
                McDuelResult* result = &app->ui.duel[app->ui.duel_player];
                *result = (McDuelResult){
                    app->ui.game.score,
                    app->ui.game.stats.play_ticks,
                    app->ui.game.stats.shots_fired,
                    app->ui.game.stats.successful_shots};
                app->ui.screen = app->ui.duel_player ? McScreenDuelResults : McScreenDuelSwap;
                mc_feedback_game_over(&app->feedback);
                break;
            }
            if(app->ui.game.mode == McModePractice) {
                mc_feedback_game_over(&app->feedback);
                break;
            }
            if(mc_game_competitive(&app->ui.game) && app->ui.game.score > app->pace.score) {
                app->pace.score = app->ui.game.score;
                memcpy(app->pace.waves, app->ui.game.wave_scores, sizeof(app->pace.waves));
                app->pending_io |= McPendingIoPaceSave;
            }
            mc_app_record_score(app);
            mc_app_queue_medals(app, mc_profile_update_game_over(&app->ui.profile, &app->ui.game));
            mc_app_profile_changed(app);
            mc_app_queue_run_delete(app);
            mc_feedback_game_over(&app->feedback);
            break;
        }
    }
    if(best_chain > 0U) mc_feedback_chain(&app->feedback, app->ui.tick, best_chain);
}

static void mc_app_step_cursor(McApp* app) {
    const int8_t dx = app->cursor_direction_x;
    const int8_t dy = app->cursor_direction_y;
    if(dx == 0 && dy == 0) return;
    const uint16_t step = mc_cursor_step_q8(
        app->ui.settings.cursor_speed,
        app->ui.settings.cursor_acceleration ? app->cursor_hold_ticks : 0U,
        dx != 0 && dy != 0);
    app->pending_changes |= mc_game_move_cursor_q8(
        &app->ui.game, (int16_t)(dx * (int16_t)step), (int16_t)(dy * (int16_t)step));
    if(app->cursor_hold_ticks < UINT16_MAX) app->cursor_hold_ticks++;
}

static void mc_app_step(McApp* app) {
    app->ui.tick++;
    if(app->ui.screen != McScreenPlaying) return;
    if(app->ui.countdown_ticks) {
        app->ui.countdown_ticks--;
        app->pending_changes |= McGameChangeAll;
        return;
    }
    if(app->ui.shot_notice_ticks) {
        app->ui.shot_notice_ticks--;
        app->pending_changes |= McGameChangeHud;
    }
    mc_app_step_cursor(app);
    // Slow Practice halves simulation speed while cursor control and UI timers run normally
    if(app->ui.game.mode == McModePractice && app->ui.game.options.slow && !(app->ui.tick & 1U)) {
        mc_app_refresh_render_cache(app);
        return;
    }
    McGameEventBuffer events;
    app->pending_changes |= mc_game_step(&app->ui.game, &events);
    mc_app_dispatch_events(app, &events);
    if((app->pending_changes & McGameChangeHud) || app->ui.game.mode == McModeSprint ||
       ((app->ui.game.mode == McModePractice || app->ui.wave_practice) &&
        app->ui.settings.practice_aids) ||
       app->ui.settings.impact_warnings)
        mc_app_refresh_render_cache(app);
}

// The GUI model is published only by the application. The worker never reads it.
static void mc_app_apply_io_result(McApp* app, McStorageResult result, uint32_t now) {
    app->ui.storage_result = result;
    mc_app_show_storage_notice(app, result, now);
    if(result != McStorageOk) app->retry_due_tick = now + furi_ms_to_ticks(MC_SAVE_DEBOUNCE_MS);
}

void mc_app_cleanup_select(McApp* app, bool next) {
    if(!app->ui.cleanup_count) return;
    if(next && app->ui.cleanup_offset + 40U < app->ui.cleanup_length)
        app->ui.cleanup_offset += 40U;
    else if(!next && app->ui.cleanup_offset)
        app->ui.cleanup_offset -= 40U;
    else {
        app->ui.cleanup_index = next ? (app->ui.cleanup_index + 1U) % app->ui.cleanup_count :
                                       (app->ui.cleanup_index ? app->ui.cleanup_index - 1U :
                                                                app->ui.cleanup_count - 1U);
        app->ui.cleanup_offset = next ? 0 : UINT16_MAX;
    }
    app->cleanup_refresh = true;
}

static void mc_app_publish_run(McApp* app, const McIoJob* j) {
    mc_feedback_stop(&app->feedback);
    app->ui.game = j->data.run.game;
    app->wave_start = j->data.run.wave;
    app->ui.has_wave_start = app->wave_start.size != 0;
    app->ui.wave_practice = false;
    app->run_identity++;
    app->ui.battery_overlay = false;
    app->ui.setup_mode = app->ui.game.mode;
    app->ui.setup_seed = app->ui.game.seed;
    app->ui.setup_options = app->ui.game.options;
    app->ui.hud_valid = app->ui.score_recorded = app->ui.new_high_score = false;
    app->ui.menu_index = app->ui.repair_site_index = app->ui.workshop_reason = 0;
    app->ui.supply_index = 1;
    for(uint8_t site = 0; site < MC_SITE_COUNT; site++) {
        if(!mc_game_site_alive(&app->ui.game, site) &&
           (mc_mode_def(app->ui.game.mode)->repair_sites & (1U << site))) {
            app->ui.repair_site_index = site;
            app->ui.supply_index = 0;
            break;
        }
    }
    mc_app_clear_directions(app);
    app->ui.screen = j->data.run.stage == McRunStageWaveResult ? McScreenWaveResult :
                     j->data.run.stage == McRunStageRepair     ? McScreenRepair :
                                                                 McScreenPaused;
    mc_app_set_high_score(app, app->ui.game.difficulty);
    mc_app_refresh_render_cache(app);
    app->pending_io |= McPendingIoPaceLoad;
    app->pending_changes |= McGameChangeAll;
}

static void mc_app_complete_io(McApp* app, uint32_t now) {
    const McIoJob* j = &app->worker.job;
    if(j->operation == McIoHelp) {
        if(app->ui.screen != McScreenCleanup && j->data.help.revision == app->ui.help.revision &&
           j->data.help.id == mc_help_requested(&app->ui.common, &app->ui.game))
            app->ui.help = j->data.help;
        return;
    }
    const McStorageResult result = j->result;
    const bool ok = result == McStorageOk;
    app->history_pending = j->history_pending;
    app->history_required = j->history_required;
    app->io_recovered = j->recovered;
    if(app->ui.exit_discard) return;
    if(j->operation == McIoHistory && j->data.cleanup.action && result == McStorageIoError)
        app->clear_history = true;
    if(j->family) {
        if(ok || result == McStorageMissing)
            app->failed_io &= (McPendingIo)~j->family;
        else if(result == McStorageBusy)
            app->pending_io |= j->family;
        else
            app->failed_io |= j->family;
    }
    if(j->operation == McIoCleanupInit || j->operation == McIoCleanupStep) {
        const McCleanupReply* r = &j->data.cleanup;
        app->ui.cleanup_state = r->state;
        app->ui.cleanup_count = r->count;
        app->ui.cleanup_remaining = r->remaining;
        app->ui.cleanup_approved = r->approved;
        app->ui.cleanup_limited = r->limited;
        app->ui.cleanup_can_migrate = r->can_migrate;
        app->ui.cleanup_migrating = r->migrating;
        memcpy(app->ui.cleanup_source, r->source, sizeof(r->source));
        // Ignore an obsolete selection while preserving scan progress.
        if(app->ui.cleanup_index == r->requested_index &&
           app->ui.cleanup_offset == r->requested_offset) {
            app->ui.cleanup_index = r->index;
            app->ui.cleanup_offset = r->offset;
            app->ui.cleanup_length = r->length;
            memcpy(app->ui.cleanup_name, r->name, sizeof(r->name));
        }
        if(r->state == McCleanupDone) {
            app->ui.help = (McHelpPage){.id = McHelpNoPage};
            if(app->cleanup_management && !r->imported) {
                app->ui.screen = McScreenSettings;
                app->ui.menu_index = McSettingsItemVersionData;
                app->startup_step = 0;
                app->cleanup_management = false;
                app->pending_io |= McPendingIoHistory;
            } else {
                app->ui.screen = McScreenLoading;
                app->ui.menu_index = 0;
                app->startup_step = 2U;
            }
        } else if(r->state == McCleanupFailed) {
            if(app->ui.menu_index > 1U) app->ui.menu_index = 1U;
            app->ui.storage_item = "Version folder cleanup";
            mc_app_apply_io_result(app, result, now);
        }
        if(r->state != McCleanupDone) {
            app->ui.screen = McScreenCleanup;
            if(j->startup) app->startup_step = 0;
        }
        return;
    }
    switch(j->operation) {
    case McIoLoadSettings:
        app->ui.settings = app->saved_settings = j->data.settings;
        if(result == McStorageInvalid) mc_app_settings_changed(app);
        app->ui.storage_item = "High scores";
        break;
    case McIoLoadScores:
        app->ui.scores = j->data.scores;
        if(result == McStorageInvalid) mc_app_save_scores(app);
        app->ui.storage_item = "Medals";
        break;
    case McIoLoadProfile:
        app->ui.profile = j->data.profile;
        if(result == McStorageInvalid) {
            mc_app_profile_changed(app);
        }
        app->ui.storage_item = "Saved run";
        break;
    case McIoLoadRun:
        if(j->run_identity == app->run_identity && j->slot == app->ui.active_slot) {
            app->ui.has_suspended_run = ok;
            if(ok) {
                app->ui.slots[j->slot].generation = j->generation;
                app->ui.slots[j->slot].timestamp = j->timestamp;
                if(!j->startup && !app->ui.exit_pending) mc_app_publish_run(app, j);
            }
            if(j->startup) {
                mc_app_refresh_high_score(app);
                app->ui.score_difficulty = app->ui.settings.difficulty;
                app->ui.screen = app->cleanup_management ? McScreenSettings : McScreenTitle;
                if(app->cleanup_management) {
                    app->ui.menu_index = McSettingsItemVersionData;
                    app->cleanup_management = false;
                    app->slot_scan = 0;
                    app->pending_io |= McPendingIoSlots;
                }
                app->pending_io |= McPendingIoHistory;
            }
        }
        break;
    case McIoSaveRun:
        if(j->run_identity == app->run_identity && j->slot == app->ui.active_slot) {
            if(ok) {
                app->ui.slots[j->slot].generation = j->generation;
                app->ui.has_suspended_run = true;
                if(j->revision == app->run_revision) app->run_dirty = false;
                if(j->return_title && j->revision == app->run_revision && !app->ui.exit_pending) {
                    mc_feedback_stop(&app->feedback);
                    app->ui.screen = McScreenTitle;
                    app->ui.menu_index = 0;
                }
            } else
                app->run_dirty = true;
        }
        break;
    case McIoDeleteRun:
        if(j->run_identity == app->run_identity && j->slot == app->ui.active_slot)
            app->ui.has_suspended_run = false;
        break;
    case McIoSaveSettings:
        if(ok)
            app->saved_settings = j->data.settings;
        else
            app->settings_due_tick = now + furi_ms_to_ticks(MC_SAVE_DEBOUNCE_MS);
        app->settings_dirty = !ok || j->revision != app->settings_revision ||
                              !mc_settings_equal(&app->ui.settings, &app->saved_settings);
        break;
    case McIoSaveScores:
        app->scores_dirty = !ok || j->revision != app->scores_revision ||
                            memcmp(&app->ui.scores, &j->data.scores, sizeof(McScoreTables));
        break;
    case McIoSaveProfile:
        app->profile_dirty = !ok || j->revision != app->profile_revision ||
                             memcmp(&app->ui.profile, &j->data.profile, sizeof(McProfile));
        break;
    case McIoLoadPace:
        if(j->run_identity == app->run_identity) {
            app->pace = j->data.pace.pace;
            mc_app_refresh_render_cache(app);
        }
        break;
    case McIoSavePace:
        if(ok) app->pending_io |= McPendingIoHistory;
        break;
    case McIoSlots:
        app->ui.slots[j->slot] = j->data.info;
        if(++app->slot_scan < MC_SAVE_SLOTS) app->pending_io |= McPendingIoSlots;
        break;
    default:
        break;
    }
    if(j->startup) app->startup_step = j->operation == McIoLoadRun ? 0U : app->startup_step + 1U;
    mc_app_apply_io_result(app, result, now);
}

static void mc_app_queue_dirty(McApp* app, bool settings_due) {
    app->pending_io |= app->failed_io;
    if(app->settings_dirty && settings_due) app->pending_io |= McPendingIoSettings;
    if(app->scores_dirty) app->pending_io |= McPendingIoScores;
    if(app->profile_dirty) app->pending_io |= McPendingIoProfile;
    if(app->run_dirty) app->pending_io |= McPendingIoRunSave;
}

static __attribute__((noinline)) bool mc_app_process_io(McApp* app, uint32_t now) {
    unsigned state = atomic_load_explicit(&app->worker.state, memory_order_acquire);
    bool changed = false;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(state == McIoComplete) {
        mc_app_complete_io(app, now);
        atomic_store_explicit(&app->worker.state, McIoIdle, memory_order_release);
        state = McIoIdle;
        changed = true;
    }
    if(app->cleanup_requested) {
        if(app->ui.screen != McScreenSettings || app->ui.menu_index != McSettingsItemVersionData ||
           app->ui.exit_pending) {
            app->cleanup_requested = false;
        } else if(state == McIoIdle && mc_app_version_data_ready(app)) {
            app->cleanup_requested = false;
            app->cleanup_management = true;
            // Writes are settled. Obsolete reads and optional history can be restarted later.
            app->pending_io = app->failed_io = McPendingIoNone;
            app->startup_step = 1;
            app->ui.cleanup_state = McCleanupScanning;
            app->ui.cleanup_confirm = false;
            app->ui.cleanup_index = app->ui.cleanup_offset = 0;
            app->ui.menu_index = 0;
            app->ui.screen = McScreenCleanup;
            changed = true;
        }
    }
    mc_help_refresh(&app->ui.common, &app->ui.game);
    furi_mutex_release(app->mutex);
    if(state != McIoIdle || app->ui.exit_discard) return changed;
    McIoJob* j = &app->worker.job;
    memset(j, 0, sizeof(*j));
    j->slot = app->ui.active_slot;
    j->generation = app->ui.slots[j->slot].generation;
    j->run_identity = app->run_identity;
    if(app->startup_step) {
        j->startup = app->startup_step;
        static const McIoOperation startup[] = {
            McIoCleanupInit, McIoLoadSettings, McIoLoadScores, McIoLoadProfile, McIoLoadRun};
        j->operation = startup[app->startup_step - 1U];
        if(j->operation == McIoCleanupInit) j->data.cleanup.manual = app->cleanup_management;
    } else if(app->ui.screen == McScreenCleanup) {
        if(app->cleanup_action || app->cleanup_refresh ||
           app->ui.cleanup_state == McCleanupScanning ||
           app->ui.cleanup_state == McCleanupPurging ||
           app->ui.cleanup_state == McCleanupMigrating ||
           app->ui.cleanup_state == McCleanupValidating) {
            j->operation = McIoCleanupStep;
            j->data.cleanup.index = app->ui.cleanup_index;
            j->data.cleanup.offset = app->ui.cleanup_offset;
            j->data.cleanup.action = app->cleanup_action;
            app->cleanup_action = 0;
            app->cleanup_refresh = false;
        } else
            return changed;
    } else {
        if(app->settings_dirty && mc_tick_reached(now, app->settings_due_tick))
            app->pending_io |= McPendingIoSettings;
        if(mc_tick_reached(now, app->retry_due_tick)) {
            mc_app_queue_dirty(app, false);
        }
        static const struct __attribute__((packed)) {
            McPendingIo family;
            McIoOperation operation;
        } priorities[] = {
            {McPendingIoRunLoad, McIoLoadRun},
            {McPendingIoRunSave, McIoSaveRun},
            {McPendingIoRunDelete, McIoDeleteRun},
            {McPendingIoPaceLoad, McIoLoadPace},
            {McPendingIoPaceSave, McIoSavePace},
            {McPendingIoSlots, McIoSlots},
            {McPendingIoSettings, McIoSaveSettings},
            {McPendingIoScores, McIoSaveScores},
            {McPendingIoProfile, McIoSaveProfile},
            {McPendingIoHistory, McIoHistory},
        };
        bool found = false;
        for(unsigned i = 0; i < sizeof(priorities) / sizeof(priorities[0]); i++) {
            if(!(app->pending_io & priorities[i].family)) continue;
            if(priorities[i].family == McPendingIoHistory &&
               (app->ui.screen == McScreenPlaying || app->ui.exit_pending) &&
               !app->history_required && !app->clear_history)
                continue;
            j->family = priorities[i].family;
            j->operation = priorities[i].operation;
            found = true;
            break;
        }
        // Optional help may precede discretionary history, never required writes.
        if((!found ||
            (j->operation == McIoHistory && !app->history_required && !app->clear_history)) &&
           app->ui.help.status == McHelpLoading && app->ui.help.id != McHelpNoPage &&
           !app->ui.exit_pending) {
            j->family = 0;
            j->operation = McIoHelp;
            j->data.help = app->ui.help;
            mc_storage_worker_submit(&app->worker);
            return true;
        }
        if(!found) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->ui.storage_pending = app->failed_io || app->settings_dirty || app->scores_dirty ||
                                      app->profile_dirty || app->run_dirty;
            if(!app->ui.storage_pending && app->ui.storage_result == McStorageBusy)
                app->ui.storage_result = McStorageOk;
            furi_mutex_release(app->mutex);
            return changed;
        }
        if(j->family == McPendingIoPaceSave && app->history_pending) {
            if((app->failed_io & McPendingIoHistory) &&
               !mc_tick_reached(now, app->retry_due_tick)) {
                app->pending_io &= (McPendingIo)~McPendingIoPaceSave;
                app->failed_io |= McPendingIoPaceSave;
                return changed;
            }
            j->family = McPendingIoHistory;
            j->operation = McIoHistory;
        }
        app->pending_io &= (McPendingIo)~j->family;
        switch(j->operation) {
        case McIoSaveSettings:
            j->data.settings = app->ui.settings;
            j->revision = app->settings_revision;
            break;
        case McIoSaveScores:
            j->data.scores = app->ui.scores;
            j->revision = app->scores_revision;
            break;
        case McIoSaveProfile:
            j->data.profile = app->ui.profile;
            j->revision = app->profile_revision;
            break;
        case McIoSaveRun:
            j->data.run.game = app->ui.game;
            j->data.run.wave = app->wave_start;
            j->data.run.stage = app->pending_run_stage;
            j->revision = app->run_revision;
            j->return_title = app->save_title_on_success;
            app->save_title_on_success = false;
            break;
        case McIoLoadPace:
        case McIoSavePace:
            j->data.pace.game = app->ui.game;
            j->data.pace.pace = app->pace;
            break;
        case McIoSlots:
            j->slot = app->slot_scan;
            break;
        case McIoHistory:
            j->data.cleanup.action = app->clear_history;
            app->clear_history = false;
            break;
        default:
            break;
        }
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->ui.storage_pending = true;
        app->ui.storage_result = McStorageBusy;
        app->ui.storage_item = j->family == McPendingIoSettings ? "Settings" :
                               j->family == McPendingIoScores   ? "High scores" :
                               j->family == McPendingIoProfile  ? "Medals" :
                               j->family == McPendingIoHistory  ? "Pace history" :
                               (j->family & (McPendingIoPaceLoad | McPendingIoPaceSave)) ?
                                                                 "Pace record" :
                                                                 "Saved run";
        furi_mutex_release(app->mutex);
    }
    mc_storage_worker_submit(&app->worker);
    return true;
}

static bool mc_app_exit_required(const McApp* app) {
    return (app->pending_io & ~McPendingIoHistory) || (app->failed_io & ~McPendingIoHistory) ||
           app->settings_dirty || app->scores_dirty || app->profile_dirty || app->run_dirty ||
           app->history_required || app->clear_history;
}

void mc_app_open_version_data(McApp* app) {
    // Runtime import reloads persistent state, so only title-menu settings may enter.
    if(app->ui.settings_return_screen != McScreenTitle) return;
    app->cleanup_requested = true;
    mc_app_flush_settings(app);
    mc_app_queue_dirty(app, true);
    app->retry_due_tick = furi_get_tick();
}

static bool mc_app_version_data_ready(const McApp* app) {
    const McPendingIo writes = McPendingIoSettings | McPendingIoScores | McPendingIoProfile |
                               McPendingIoRunSave | McPendingIoRunDelete | McPendingIoPaceSave;
    return !((app->pending_io | app->failed_io) & writes) && !app->settings_dirty &&
           !app->scores_dirty && !app->profile_dirty && !app->run_dirty &&
           !app->history_required && !app->clear_history;
}

// Use the earliest active deadline so waiting for input cannot delay simulation, drawing, or saves
static uint32_t mc_app_wait_timeout(
    const McApp* app,
    uint32_t now,
    uint32_t simulation_accumulator,
    uint32_t tick_frequency) {
    const unsigned state = atomic_load_explicit(&app->worker.state, memory_order_acquire);
    if(state == McIoComplete || state == McIoStopped) return 0;
    if(app->startup_step && state == McIoIdle) return 0;
    if(app->cleanup_requested && state == McIoIdle && mc_app_version_data_ready(app)) return 0;
    if(app->ui.screen == McScreenCleanup && state == McIoIdle &&
       (app->cleanup_action || app->cleanup_refresh ||
        app->ui.cleanup_state == McCleanupScanning || app->ui.cleanup_state == McCleanupPurging ||
        app->ui.cleanup_state == McCleanupMigrating ||
        app->ui.cleanup_state == McCleanupValidating))
        return furi_ms_to_ticks(1U) + 1U;
    if(state == McIoIdle && app->pending_io != McPendingIoNone &&
       app->ui.screen != McScreenPlaying &&
       (!app->ui.exit_pending || (app->pending_io & ~McPendingIoHistory) || app->history_required))
        return 0U;
    if(app->ui.exit_pending && state == McIoIdle &&
       (app->ui.exit_discard || !mc_app_exit_required(app)))
        return 0U;
    uint32_t timeout = FuriWaitForever;
    if(app->ui.screen == McScreenPlaying) {
        const uint32_t remaining =
            simulation_accumulator < tick_frequency ? tick_frequency - simulation_accumulator : 0U;
        // Round up from 30-Hz units to avoid waking before a step is due
        timeout = (remaining + 29U) / 30U;
        if(app->pending_changes != McGameChangeNone) {
            const uint32_t frame_wait =
                mc_render_clock_wait(&app->render_clock, now, tick_frequency);
            if(frame_wait < timeout) timeout = frame_wait;
        }
    }
    if(state == McIoIdle && app->ui.screen != McScreenPlaying && app->settings_dirty) {
        const uint32_t wait = mc_tick_until(now, app->settings_due_tick);
        if(timeout == FuriWaitForever || wait < timeout) timeout = wait;
    }
    if(state == McIoIdle && app->ui.screen != McScreenPlaying &&
       (app->scores_dirty || app->profile_dirty || app->run_dirty || app->failed_io)) {
        const uint32_t wait = mc_tick_until(now, app->retry_due_tick);
        if(timeout == FuriWaitForever || wait < timeout) timeout = wait;
    }
    if(app->ui.exit_pending && !app->ui.exit_slow) {
        const uint32_t wait = mc_tick_until(now, app->exit_started + furi_ms_to_ticks(3000U));
        if(timeout == FuriWaitForever || wait < timeout) timeout = wait;
    }
    if(app->ui.storage_notice_visible) {
        const uint32_t wait = mc_tick_until(now, app->notice_expiry_tick);
        if(timeout == FuriWaitForever || wait < timeout) timeout = wait;
    }
    return timeout;
}

static void mc_view_draw_callback(Canvas* canvas, void* context) {
    McApp* app = context;
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk) {
        mc_render_snapshot(app->render_snapshot, &app->ui);
        furi_mutex_release(app->mutex);
        mc_render(canvas, app->render_snapshot);
    }
}

// One FIFO preserves callback arrival order. Physical state survives a rejected burst.
static void mc_view_input_callback(InputEvent* event, void* context) {
    McApp* app = context;
    if(event->key > InputKeyBack) return;
    const unsigned bit = 1U << event->key;
    if(event->type == InputTypePress) atomic_fetch_or(&app->physical_keys, bit);
    if(event->type == InputTypeRelease) atomic_fetch_and(&app->physical_keys, ~bit);
    if(atomic_load(&app->input_overflowed)) goto wake;
    if(event->type == InputTypeRepeat && furi_message_queue_get_space(app->input_queue) <= 8U)
        return;
    if(event->type == InputTypePress) atomic_fetch_or(&app->admitted_keys, bit);
    if(event->type == InputTypeShort || event->type == InputTypeLong) {
        if(!(atomic_load(&app->admitted_keys) & bit)) return;
        atomic_fetch_and(&app->admitted_keys, ~bit);
    }
    if(furi_message_queue_put(app->input_queue, event, 0U) != FuriStatusOk &&
       event->type != InputTypeRepeat)
        atomic_store(&app->input_overflowed, 1U);
wake:
    furi_thread_flags_set(app->worker.owner, MC_WAKE_INPUT);
}

static bool mc_app_reconcile_input(McApp* app) {
    unsigned state = atomic_load(&app->input_overflowed);
    if(!state) return false;
    if(state == 1U) {
        furi_message_queue_reset(app->input_queue);
        atomic_store(&app->admitted_keys, 0U);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->held_directions = 0U;
        app->ui.battery_overlay = false;
        mc_app_direction_changed(app, false);
        if(app->ui.screen == McScreenPlaying) {
            app->ui.screen = McScreenPaused;
            app->ui.menu_index = 0;
        }
        app->pending_changes |= McGameChangeAll;
        furi_mutex_release(app->mutex);
        atomic_store(&app->input_overflowed, 2U);
    }
    if(!atomic_load(&app->physical_keys)) atomic_store(&app->input_overflowed, 0U);
    return true;
}
static FuriStatus mc_app_get_input(McApp* app, InputEvent* event, uint32_t timeout) {
    if(mc_app_reconcile_input(app)) return FuriStatusErrorTimeout;
    if(furi_message_queue_get(app->input_queue, event, 0U) == FuriStatusOk) return FuriStatusOk;
    furi_thread_flags_wait(MC_WAKE_INPUT | MC_WAKE_IO, FuriFlagWaitAny, timeout);
    if(mc_app_reconcile_input(app)) return FuriStatusErrorTimeout;
    return furi_message_queue_get(app->input_queue, event, 0U);
}

static __attribute__((noinline)) McApp* mc_app_alloc(void) {
    McApp* app = malloc(sizeof(McApp));
    furi_check(app);
    memset(app, 0, sizeof(*app));
    atomic_init(&app->physical_keys, 0U);
    atomic_init(&app->input_overflowed, 0U);
    atomic_init(&app->admitted_keys, 0U);

    app->render_snapshot = malloc(sizeof(McRenderSnapshot));
    furi_check(app->render_snapshot);
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(MC_INPUT_QUEUE_CAPACITY, sizeof(InputEvent));
    app->gui = furi_record_open(RECORD_GUI);
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);
    furi_check(mc_persistence_init(&app->persistence, app->storage));
    mc_storage_worker_init(&app->worker, &app->persistence, &app->cleanup);
    app->history_pending = true;
    app->run_identity = 1;

    mc_game_init(&app->ui.game);
    mc_settings_defaults(&app->ui.settings);
    mc_score_tables_defaults(&app->ui.scores);
    mc_profile_defaults(&app->ui.profile);
    const uint32_t now = furi_get_tick();
    app->startup_step = 1;
    app->ui.screen = McScreenLoading;
    app->ui.cleanup_state = McCleanupScanning;
    app->ui.storage_item = "Settings";
    app->ui.settings_return_screen = McScreenTitle;
    app->ui.scores_return_screen = McScreenTitle;
    app->ui.detail_return_screen = McScreenHighScores;
    app->ui.confirm_return_screen = McScreenTitle;
    app->ui.score_difficulty = app->ui.settings.difficulty;
    app->running = true;
    app->retry_due_tick = now;
    app->pending_changes = McGameChangeAll;
    mc_app_refresh_render_cache(app);
    mc_feedback_init(&app->feedback, app->notification, &app->ui.settings);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, mc_view_draw_callback, app);
    view_port_input_callback_set(app->view_port, mc_view_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    return app;
}

void mc_app_request_exit(McApp* app) {
    app->ui.exit_pending = true;
    app->ui.exit_slow = false;
    app->pending_io &=
        (McPendingIo) ~(McPendingIoRunLoad | McPendingIoPaceLoad | McPendingIoSlots);
    app->failed_io &= (McPendingIo) ~(McPendingIoRunLoad | McPendingIoPaceLoad | McPendingIoSlots);
    app->ui.exit_discard = false;
    app->exit_started = furi_get_tick();
    app->ui.screen = McScreenExiting;
    app->ui.menu_index = 0;
    mc_app_flush_settings(app);
    app->retry_due_tick = furi_get_tick();
    app->pending_changes |= McGameChangeAll;
}
static void mc_app_process_exit(McApp* app, uint32_t now) {
    if(!app->ui.exit_pending) return;
    const unsigned state = atomic_load_explicit(&app->worker.state, memory_order_acquire);
    if(state == McIoStopped) {
        app->running = false;
        return;
    }
    if(!app->ui.exit_slow && now - app->exit_started >= furi_ms_to_ticks(3000U)) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->ui.exit_slow = true;
        app->pending_changes |= McGameChangeAll;
        furi_mutex_release(app->mutex);
    }
    if(state == McIoIdle && (app->ui.exit_discard || !mc_app_exit_required(app)))
        mc_storage_worker_stop(&app->worker);
}

static void mc_app_free(McApp* app) {
    // Test helpers may tear down an idle app without going through the exit screen.
    unsigned state = atomic_load_explicit(&app->worker.state, memory_order_acquire);
    while(state == McIoPending) {
        furi_thread_flags_wait(MC_WAKE_IO, FuriFlagWaitAny, FuriWaitForever);
        state = atomic_load_explicit(&app->worker.state, memory_order_acquire);
    }
    if(state == McIoIdle || state == McIoComplete) mc_storage_worker_stop(&app->worker);
    while(atomic_load_explicit(&app->worker.state, memory_order_acquire) != McIoStopped)
        furi_thread_flags_wait(MC_WAKE_IO, FuriFlagWaitAny, FuriWaitForever);
    mc_storage_worker_free(&app->worker);
    mc_feedback_deinit(&app->feedback);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    free(app->render_snapshot);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);
}

// Input, 30-Hz simulation, frame pacing, and deferred storage share this loop
int32_t mc_app_run(void) {
    McApp* app = mc_app_alloc();
    const uint32_t tick_frequency = furi_kernel_get_tick_frequency();
    uint32_t simulation_accumulator = 0U;
    uint32_t previous_tick = furi_get_tick();

    while(app->running) {
        const uint32_t before_wait = furi_get_tick();
        const bool was_playing = app->ui.screen == McScreenPlaying;
        const uint32_t timeout =
            mc_app_wait_timeout(app, before_wait, simulation_accumulator, tick_frequency);
        InputEvent event;
        FuriStatus input_status = mc_app_get_input(app, &event, timeout);
        const uint32_t now = furi_get_tick();
        const uint32_t elapsed = now - previous_tick;
        previous_tick = now;

        bool immediate_redraw = false;
        for(unsigned batch = 0; batch < 8U && input_status == FuriStatusOk; batch++) {
            if(mc_app_reconcile_input(app)) break;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            const McScreen prior_screen = app->ui.screen;
            mc_app_handle_input(app, &event);
            immediate_redraw |= app->ui.screen != prior_screen ||
                                app->ui.screen != McScreenPlaying;
            if(immediate_redraw) app->pending_changes |= McGameChangeAll;
            mc_app_refresh_render_cache(app);
            furi_mutex_release(app->mutex);
            if(batch + 1U < 8U)
                input_status = furi_message_queue_get(app->input_queue, &event, 0U);
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        const bool playing = app->ui.screen == McScreenPlaying;
        const uint8_t steps = mc_runtime_steps(
            &simulation_accumulator, elapsed, tick_frequency, was_playing, playing);
        furi_mutex_release(app->mutex);

        for(uint8_t step = 0U; step < steps; step++) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            mc_app_step(app);
            furi_mutex_release(app->mutex);
            if(app->ui.screen != McScreenPlaying) {
                simulation_accumulator = 0U;
                break;
            }
        }

        // Keep storage outside the render mutex; lock only to publish UI results
        const bool io_changed = mc_app_process_io(app, now);
        mc_app_process_exit(app, now);
        bool notice_expired = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->ui.storage_notice_visible && mc_tick_reached(now, app->notice_expiry_tick)) {
            app->ui.storage_notice_visible = false;
            notice_expired = true;
        }
        const McGameChange changes = app->pending_changes;
        const bool phase_change = (changes & McGameChangePhase) != 0U;
        uint8_t render_fps = 0U;
        if(app->ui.screen == McScreenPlaying)
            render_fps = mc_render_target_fps(&app->ui.game, app->ui.settings.render_mode);
        // Cursor motion needs frames even when no missiles or effects are active
        if((changes != McGameChangeNone || app->held_directions || app->ui.countdown_ticks ||
            app->ui.shot_notice_ticks) &&
           render_fps == 0U)
            render_fps = app->ui.settings.render_mode == McRenderBatterySaver ? 15U : 30U;
        const bool render_due =
            mc_render_clock_advance(
                &app->render_clock, furi_get_tick(), tick_frequency, render_fps) &&
            changes != McGameChangeNone;
        const bool redraw = immediate_redraw || io_changed || notice_expired || phase_change ||
                            (app->ui.screen == McScreenPlaying && render_due) ||
                            (app->ui.screen != McScreenPlaying && changes != McGameChangeNone);
        if(redraw) {
            app->pending_changes = McGameChangeNone;
        }
        furi_mutex_release(app->mutex);
        if(redraw) view_port_update(app->view_port);
    }

    mc_app_free(app);
    return 0;
}

void mc_app_retry(McApp* app, bool random_seed) {
    if(app->ui.wave_practice) {
        mc_app_practice_wave(app);
        return;
    }
    app->ui.remember_setup = app->ui.override_difficulty = false;
    app->ui.setup_mode = app->ui.game.mode;
    app->ui.setup_seed = random_seed ? furi_hal_random_get() : app->ui.game.seed;
    if(!app->ui.setup_seed) app->ui.setup_seed = 1U;
    app->ui.setup_options = app->ui.game.options;
    if(random_seed && mc_mode_def(app->ui.setup_mode)->seed_policy != McSeedRandom) {
        app->ui.setup_mode = McModeClassic;
        memset(&app->ui.setup_options, 0, sizeof(app->ui.setup_options));
    }
    app->ui.retry_seed = true;
    mc_app_start_new_run(app, false);
}
void mc_app_continue(McApp* app) {
    mc_app_clear_directions(app);
    app->ui.countdown_ticks = app->ui.settings.resume_countdown ? 3U * MC_TICKS_PER_SECOND : 0U;
    app->ui.screen = McScreenPlaying;
    app->pending_changes |= McGameChangeAll;
}
void mc_app_retry_storage(McApp* app) {
    app->retry_due_tick = 0U;
    app->settings_due_tick = 0U;
    mc_app_queue_dirty(app, true);
}
void mc_app_select_slot(McApp* app, uint8_t slot) {
    if(slot >= MC_SAVE_SLOTS || app->ui.slots[slot].status == McStorageBusy) return;
    if(mc_app_wait_for_required_io(app)) return;
    app->ui.active_slot = slot;
    app->run_identity++;
    app->ui.has_suspended_run = app->ui.slots[slot].status == McStorageOk;
    app->ui.screen = McScreenTitle;
    app->ui.menu_index = 0U;
}

void mc_app_play_last(McApp* app) {
    const McSettings* settings = &app->ui.settings;
    if(!settings->last_setup_valid) return;
    app->ui.setup_mode = settings->last_mode;
    app->ui.setup_options = settings->last_options;
    app->ui.setup_difficulty = settings->last_difficulty;
    app->ui.override_difficulty = true;
    app->ui.remember_setup = app->ui.retry_seed = false;
    app->ui.setup_seed = settings->last_mode == McModeSeeded ? settings->last_seed :
                                                               furi_hal_random_get();
    if(!app->ui.setup_seed) app->ui.setup_seed = 1U;
    app->ui.duel_player = 0U;
    if(app->ui.has_suspended_run && mc_mode_has(settings->last_mode, McModeSave))
        mc_app_confirm(app, McConfirmOverwriteRun, McScreenTitle);
    else
        mc_app_start_new_run(app, true);
}
void mc_app_capture_wave(McApp* app) {
    app->ui.has_wave_start = mc_mode_has(app->ui.game.mode, McModeReplay) &&
                             mc_wave_start_capture(&app->wave_start, &app->ui.game);
    if(!app->ui.has_wave_start) app->wave_start.size = 0;
}
void mc_app_practice_wave(McApp* app) {
    if(!app->ui.has_wave_start || mc_app_wait_for_required_io(app)) return;
    if(!mc_wave_start_restore(&app->wave_start, &app->ui.game)) return;
    mc_feedback_stop(&app->feedback);
    app->ui.wave_practice = true;
    app->ui.menu_index = app->ui.shot_notice_ticks = 0;
    app->ui.battery_overlay = false;
    app->ui.hud_valid = false;
    app->ui.high_score = 0;
    mc_app_continue(app);
    mc_app_refresh_render_cache(app);
    app->pending_changes |= McGameChangeAll;
}

void mc_app_clear_pace(McApp* app) {
    app->pending_io &= (McPendingIo) ~(McPendingIoPaceSave | McPendingIoPaceLoad);
    app->failed_io &=
        (McPendingIo) ~(McPendingIoPaceSave | McPendingIoPaceLoad | McPendingIoHistory);
    memset(&app->pace, 0, sizeof(app->pace));
    app->ui.pace_available = false;
    app->clear_history = true;
    app->pending_io |= McPendingIoHistory;
}

void mc_app_profile_changed(McApp* app) {
    app->profile_dirty = true;
    app->profile_revision++;
    app->pending_io |= McPendingIoProfile;
}

void mc_app_clear_directions(McApp* app) {
    app->held_directions = 0U;
    mc_app_direction_changed(app, false);
}
