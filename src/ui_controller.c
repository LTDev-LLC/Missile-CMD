#include "app_internal.h"

#include "controls.h"
#include "help_ui.h"
#include "ui_menu.h"
#include <furi_hal.h>
#include <string.h>

static bool mc_is_direction_event(const InputEvent* event) {
    return (event->type == InputTypePress || event->type == InputTypeRepeat) &&
           (event->key == InputKeyLeft || event->key == InputKeyRight ||
            event->key == InputKeyUp || event->key == InputKeyDown);
}

static bool mc_is_direction_key(InputKey key) {
    return key == InputKeyLeft || key == InputKeyRight || key == InputKeyUp || key == InputKeyDown;
}

static uint8_t mc_direction_mask(InputKey key) {
    switch(key) {
    case InputKeyLeft:
        return MC_HELD_LEFT;
    case InputKeyRight:
        return MC_HELD_RIGHT;
    case InputKeyUp:
        return MC_HELD_UP;
    case InputKeyDown:
        return MC_HELD_DOWN;
    default:
        return 0U;
    }
}

static bool mc_is_confirm_event(const InputEvent* event) {
    return event->type == InputTypeShort && event->key == InputKeyOk;
}

static bool mc_is_back_event(const InputEvent* event) {
    return event->type == InputTypeShort && event->key == InputKeyBack;
}

static bool mc_is_long_back_event(const InputEvent* event) {
    return event->type == InputTypeLong && event->key == InputKeyBack;
}

static uint8_t mc_move_index(uint8_t index, uint8_t count, InputKey key) {
    if(key != InputKeyUp && key != InputKeyDown) return index;
    return mc_wrap_step(index, count, key == InputKeyDown);
}

static void mc_move_menu(McApp* app, const InputEvent* event, McMenuKind menu) {
    app->ui.menu_index = mc_move_index(
        app->ui.menu_index,
        mc_menu_count(menu, mc_menu_context(&app->ui.common, &app->ui.game)),
        event->key);
}

static void mc_open_hud_guide(McApp* app, McScreen return_screen) {
    app->ui.detail_return_screen = return_screen;
    app->ui.guide_page = 0U;
    app->ui.screen = McScreenHudGuide;
}

static __attribute__((noinline)) void mc_handle_title_input(McApp* app, const InputEvent* event) {
    const McMenuAction action = mc_menu_action(
        McMenuTitle, mc_menu_context(&app->ui.common, &app->ui.game), app->ui.menu_index);
    if(mc_is_direction_event(event)) {
        if(event->key == InputKeyUp || event->key == InputKeyDown) {
            mc_move_menu(app, event, McMenuTitle);
        }
    } else if(mc_is_confirm_event(event)) {
        switch(action) {
        case McMenuActionResume:
            mc_app_resume_run(app);
            break;
        case McMenuActionPlayLast:
            mc_app_play_last(app);
            break;
        case McMenuActionNewGame:
            mc_app_open_run_setup(app);
            break;
        case McMenuActionSlots:
            app->ui.screen = McScreenSlots;
            app->ui.menu_index = app->ui.active_slot;
            app->slot_scan = 0;
            for(unsigned i = 0; i < MC_SAVE_SLOTS; i++)
                app->ui.slots[i].status = McStorageBusy;
            app->pending_io |= McPendingIoSlots;
            break;
        case McMenuActionHighScores:
            mc_app_open_scores(app, McScreenTitle);
            break;
        case McMenuActionRecords:
            app->ui.record_index = 0U;
            app->ui.screen = McScreenRecords;
            break;
        case McMenuActionSettings:
            mc_app_open_settings(app, McScreenTitle);
            break;
        case McMenuActionHudGuide:
            mc_open_hud_guide(app, McScreenTitle);
            break;
        case McMenuActionAbout:
            app->ui.screen = McScreenAbout;
            break;
        case McMenuActionExit:
            mc_app_request_exit(app);
            break;
        default:
            break;
        }
    } else if(mc_is_back_event(event) || mc_is_long_back_event(event)) {
        mc_app_request_exit(app);
    }
}

static void mc_handle_control_card_input(McApp* app, const InputEvent* event) {
    if(mc_is_confirm_event(event)) {
        app->held_directions = 0U;
        app->ui.screen = McScreenPlaying;
    } else if(mc_is_back_event(event)) {
        app->ui.screen = McScreenTitle;
        app->ui.menu_index = 0U;
    }
}

// Short OK fires; holding OK routes directions to battery selection until release
static void mc_handle_playing_input(McApp* app, const InputEvent* event) {
    if(app->ui.countdown_ticks) {
        if(mc_is_back_event(event)) {
            app->ui.countdown_ticks = 0U;
            app->ui.screen = McScreenPaused;
        }
        return;
    }
    if(app->ui.battery_overlay) {
        if(event->key == InputKeyOk && event->type == InputTypeRelease) {
            app->ui.battery_overlay = false;
            app->pending_changes |= McGameChangeAll;
        } else if(mc_is_direction_event(event)) {
            McBatterySelection selection = McBatteryAuto;
            if(event->key == InputKeyLeft)
                selection = McBatteryLeft;
            else if(event->key == InputKeyUp)
                selection = McBatteryCenter;
            else if(event->key == InputKeyRight)
                selection = McBatteryRight;
            if(mc_game_select_battery(&app->ui.game, selection)) {
                app->pending_changes |= McGameChangeHud | McGameChangeSites;
            } else
                mc_feedback_empty(&app->feedback);
        }
        return;
    }
    if(mc_is_direction_key(event->key)) {
        const uint8_t mask = mc_direction_mask(event->key);
        if(event->type == InputTypePress || event->type == InputTypeRepeat) {
            app->held_directions |= mask;
            mc_app_direction_changed(app, event->type == InputTypePress);
        } else if(event->type == InputTypeRelease || event->type == InputTypeShort) {
            app->held_directions &= (uint8_t)~mask;
            mc_app_direction_changed(app, false);
        }
    } else if(mc_is_confirm_event(event)) {
        const McFireResult fired = mc_game_fire(&app->ui.game);
        if(fired == McFireSuccess) {
            app->pending_changes |= McGameChangeObjects | McGameChangeHud;
            mc_feedback_launch(&app->feedback);
        } else {
            app->ui.shot_reason = fired;
            app->ui.shot_notice_ticks = 30U;
            app->pending_changes |= McGameChangeAll;
            mc_feedback_empty(&app->feedback);
        }
    } else if(event->type == InputTypeLong && event->key == InputKeyOk) {
        app->ui.battery_overlay = true;
        mc_app_clear_directions(app);
        app->pending_changes |= McGameChangeAll;
    } else if(mc_is_back_event(event)) {
        mc_app_clear_directions(app);
        app->ui.screen = McScreenPaused;
        app->ui.menu_index = 0U;
        mc_app_queue_run_save(app, McRunStageActive, false);
    } else if(mc_is_long_back_event(event)) {
        mc_app_clear_directions(app);
        mc_app_confirm(app, McConfirmSaveTitle, McScreenPlaying);
    }
}

static __attribute__((noinline)) void mc_handle_paused_input(McApp* app, const InputEvent* event) {
    if(mc_is_direction_event(event)) {
        mc_move_menu(app, event, McMenuPause);
    } else if(mc_is_confirm_event(event)) {
        const McMenuAction action = mc_menu_action(
            McMenuPause, mc_menu_context(&app->ui.common, &app->ui.game), app->ui.menu_index);
        switch(action) {
        case McMenuActionContinue:
            mc_app_continue(app);
            break;
        case McMenuActionSaveTitle:
            mc_app_confirm(app, McConfirmSaveTitle, McScreenPaused);
            break;
        case McMenuActionRestart:
            mc_app_confirm(app, McConfirmRestartRun, McScreenPaused);
            break;
        case McMenuActionSettings:
            mc_app_open_settings(app, McScreenPaused);
            break;
        case McMenuActionHudGuide:
            mc_open_hud_guide(app, McScreenPaused);
            break;
        case McMenuActionPuzzleHint:
            app->ui.detail_return_screen = McScreenPaused;
            app->ui.screen = McScreenPuzzleHint;
            break;
        case McMenuActionStorage:
            app->ui.detail_return_screen = McScreenPaused;
            app->ui.screen = McScreenStorage;
            break;
        case McMenuActionAbandon:
            mc_app_confirm(app, McConfirmAbandonRun, McScreenPaused);
            break;
        default:
            break;
        }
    } else if(mc_is_back_event(event)) {
        mc_app_continue(app);
    } else if(mc_is_long_back_event(event)) {
        mc_app_confirm(app, McConfirmSaveTitle, McScreenPaused);
    }
}

static uint8_t mc_find_destroyed_site(const McGame* game, uint8_t start, bool increment) {
    uint8_t candidate = start;
    for(uint8_t attempt = 0U; attempt < MC_SITE_COUNT; attempt++) {
        candidate = mc_wrap_step(candidate, MC_SITE_COUNT, increment);
        // One City permits rebuilding only the center city and the three batteries
        if(!mc_game_site_alive(game, candidate) &&
           (mc_mode_def(game->mode)->repair_sites & (1U << candidate)))
            return candidate;
    }
    return start;
}

static void mc_begin_next_wave(McApp* app) {
    app->ui.medal_notice_mask = 0U;

    mc_game_begin_next_wave(&app->ui.game);
    mc_app_capture_wave(app);
    const uint16_t previous_wave = app->ui.profile.best_wave;
    const uint16_t medals = mc_profile_update_started(&app->ui.profile, &app->ui.game);
    app->ui.medal_notice_mask |= medals;
    if(medals || previous_wave != app->ui.profile.best_wave) mc_app_profile_changed(app);
    mc_app_clear_directions(app);
    app->ui.screen = app->ui.game.mode == McModeTraining ? McScreenLesson : McScreenPlaying;
    app->pending_changes |= McGameChangeAll;
    mc_app_queue_run_save(app, McRunStageActive, false);
}

static void mc_handle_wave_result_input(McApp* app, const InputEvent* event) {
    if(mc_is_direction_event(event) &&
       (event->key == InputKeyLeft || event->key == InputKeyRight)) {
        app->ui.stats_page = (app->ui.stats_page + 1U) % 3U;
    } else if(mc_is_confirm_event(event)) {
        if(app->ui.game.mode == McModeTraining && !mc_training_passed(&app->ui.game)) {
            mc_begin_next_wave(app);
        } else if(app->ui.game.mode == McModeTraining && app->ui.game.wave >= 4U) {
            mc_app_queue_run_delete(app);
            app->ui.screen = McScreenTitle;
            app->ui.menu_index = 0U;
        } else if(app->ui.game.repair_credits > 0U) {
            app->ui.repair_site_index = mc_find_destroyed_site(&app->ui.game, 0U, true);
            app->ui.workshop_reason = McWorkshopOk;
            app->ui.supply_index = mc_game_destroyed_sites(&app->ui.game) > 0U ? 0U : 1U;
            app->ui.screen = McScreenRepair;
            mc_app_queue_run_save(app, McRunStageRepair, false);
        } else {
            mc_begin_next_wave(app);
        }
    } else if(mc_is_back_event(event) || mc_is_long_back_event(event)) {
        mc_app_confirm(app, McConfirmSaveTitle, McScreenWaveResult);
    }
}

static void mc_handle_repair_input(McApp* app, const InputEvent* event) {
    if(mc_is_direction_event(event)) app->ui.workshop_reason = McWorkshopOk;
    if(mc_is_direction_event(event) && (event->key == InputKeyUp || event->key == InputKeyDown)) {
        app->ui.supply_index = event->key == InputKeyDown ?
                                   mc_wrap_increment(app->ui.supply_index, McSupplyCount + 1U) :
                                   mc_wrap_decrement(app->ui.supply_index, McSupplyCount + 1U);
    } else if(mc_is_direction_event(event) && event->key == InputKeyLeft) {
        app->ui.repair_site_index =
            mc_find_destroyed_site(&app->ui.game, app->ui.repair_site_index, false);
    } else if(mc_is_direction_event(event) && event->key == InputKeyRight) {
        app->ui.repair_site_index =
            mc_find_destroyed_site(&app->ui.game, app->ui.repair_site_index, true);
    } else if(mc_is_confirm_event(event)) {
        const bool repair = app->ui.supply_index == 0U;
        const McSupply supply = app->ui.supply_index - 1U;
        app->ui.workshop_reason =
            repair ? mc_game_repair_reason(&app->ui.game, app->ui.repair_site_index) :
                     mc_game_supply_reason(&app->ui.game, supply);
        if(app->ui.workshop_reason) {
            mc_feedback_empty(&app->feedback);
        } else {
            if(repair) {
                mc_game_repair_site(&app->ui.game, app->ui.repair_site_index);
                app->ui.repair_site_index =
                    mc_find_destroyed_site(&app->ui.game, app->ui.repair_site_index, true);
            } else {
                mc_game_buy_supply(&app->ui.game, supply);
            }
            app->pending_changes |= McGameChangeSites | McGameChangeHud;
            mc_app_queue_run_save(app, McRunStageRepair, false);
        }
    } else if(mc_is_back_event(event)) {
        // Workshop lesson bits: 8 for repair, 16 for supply purchase
        if(app->ui.game.mode == McModeTraining && app->ui.game.wave == 3U &&
           (app->ui.game.training_flags & 24U) != 24U)
            mc_feedback_empty(&app->feedback);
        else
            mc_begin_next_wave(app);
    } else if(mc_is_long_back_event(event)) {
        mc_app_confirm(app, McConfirmSaveTitle, McScreenRepair);
    }
}

static void mc_handle_game_over_input(McApp* app, const InputEvent* event) {
    if(mc_is_direction_event(event)) {
        mc_move_menu(app, event, McMenuGameOver);
    } else if(mc_is_confirm_event(event)) {
        const McMenuAction action = mc_menu_action(
            McMenuGameOver, mc_menu_context(&app->ui.common, &app->ui.game), app->ui.menu_index);
        if(action == McMenuActionRetry || action == McMenuActionRandom) {
            app->ui.duel_player = 0U;
            mc_app_retry(app, action == McMenuActionRandom);
        } else if(action == McMenuActionPracticeWave) {
            mc_app_practice_wave(app);
        } else if(action == McMenuActionNewGame) {
            mc_app_open_run_setup(app);
        } else if(action == McMenuActionRunStats) {
            app->ui.detail_return_screen = McScreenGameOver;
            app->ui.stats_page = 0U;
            app->ui.screen = McScreenRunStats;
        } else if(action == McMenuActionHighScores) {
            mc_app_open_scores(app, McScreenGameOver);
        } else if(action == McMenuActionTitle) {
            app->ui.screen = McScreenTitle;
            app->ui.menu_index = 0U;
        }
    } else if(mc_is_back_event(event)) {
        app->ui.screen = McScreenTitle;
        app->ui.menu_index = 0U;
    }
}

static void mc_handle_scores_input(McApp* app, const InputEvent* event) {
    if(mc_is_direction_event(event)) {
        if(event->key == InputKeyLeft) {
            app->ui.score_difficulty = (McDifficulty)mc_wrap_decrement(
                (uint8_t)app->ui.score_difficulty, McDifficultyCount);
        } else if(event->key == InputKeyRight) {
            app->ui.score_difficulty = (McDifficulty)mc_wrap_increment(
                (uint8_t)app->ui.score_difficulty, McDifficultyCount);
        } else if(event->key == InputKeyUp) {
            app->ui.score_index = mc_wrap_decrement(app->ui.score_index, MC_SCORE_CAPACITY);
        } else if(event->key == InputKeyDown) {
            app->ui.score_index = mc_wrap_increment(app->ui.score_index, MC_SCORE_CAPACITY);
        }
    } else if(mc_is_confirm_event(event)) {
        const McScoreBoard* board = mc_score_board(&app->ui.scores, app->ui.score_difficulty);
        if(board && board->entries[app->ui.score_index].score > 0U) {
            app->ui.detail_return_screen = McScreenHighScores;
            app->ui.screen = McScreenScoreDetails;
        }
    } else if(mc_is_back_event(event)) {
        app->ui.screen = app->ui.scores_return_screen;
        app->ui.menu_index = 0U;
    }
}

static void mc_toggle_setting(McApp* app, bool increment) {
    const uint8_t item = app->ui.menu_index;
    if(!mc_setting_change(&app->ui.common, item, increment)) return;
    mc_app_settings_changed(app);
    if(item == McSettingsItemLed || (item == McSettingsItemSound && !app->ui.settings.sound) ||
       (item == McSettingsItemVibration && !app->ui.settings.vibration))
        mc_feedback_stop(&app->feedback);
    if(item == McSettingsItemSound && app->ui.settings.sound)
        mc_feedback_preview_sound(&app->feedback);
    if((item == McSettingsItemVibration && app->ui.settings.vibration) ||
       item == McSettingsItemVibrationIntensity)
        mc_feedback_preview_vibration(&app->feedback);
}

static __attribute__((noinline)) void
    mc_handle_settings_input(McApp* app, const InputEvent* event) {
    if(mc_is_direction_event(event)) {
        uint8_t row = 0U;
        const uint8_t count = mc_settings_group_count(app->ui.settings_group);
        while(row < count &&
              mc_settings_group_item(app->ui.settings_group, row) != app->ui.menu_index)
            row++;
        if(event->key == InputKeyUp) {
            app->ui.menu_index =
                mc_settings_group_item(app->ui.settings_group, mc_wrap_decrement(row, count));
        } else if(event->key == InputKeyDown) {
            app->ui.menu_index =
                mc_settings_group_item(app->ui.settings_group, mc_wrap_increment(row, count));
        } else if(event->key == InputKeyLeft) {
            mc_toggle_setting(app, false);
        } else if(event->key == InputKeyRight) {
            mc_toggle_setting(app, true);
        }
    } else if(mc_is_confirm_event(event)) {
        if(app->ui.menu_index == McSettingsItemVersionData) {
            mc_app_open_version_data(app);
        } else if(app->ui.menu_index == McSettingsItemStorage) {
            app->ui.detail_return_screen = McScreenSettings;
            app->ui.screen = McScreenStorage;
        } else if(app->ui.menu_index == McSettingsItemResetSettings) {
            mc_app_confirm(app, McConfirmResetSettings, McScreenSettings);
        } else if(app->ui.menu_index == McSettingsItemClearPace) {
            mc_app_confirm(app, McConfirmClearPace, McScreenSettings);
        } else if(app->ui.menu_index == McSettingsItemResetScores) {
            mc_app_confirm(app, McConfirmResetScores, McScreenSettings);
        } else {
            mc_toggle_setting(app, true);
        }
    } else if(mc_is_back_event(event)) {
        mc_app_flush_settings(app);
        app->ui.settings_rows[app->ui.settings_group] = app->ui.menu_index;
        app->ui.menu_index = app->ui.settings_group;
        app->ui.screen = McScreenSettingsGroups;
    }
}

static void mc_handle_records_input(McApp* app, const InputEvent* event) {
    if(mc_is_direction_event(event)) {
        if(event->key == InputKeyRight || event->key == InputKeyLeft) {
            app->ui.screen = McScreenModeRecords;
            app->ui.record_index = 0U;
        } else
            app->ui.record_index = mc_move_index(app->ui.record_index, 6U, event->key);
    } else if(mc_is_confirm_event(event)) {
        app->ui.medal_index = 0U;
        app->ui.screen = McScreenMedals;
    } else if(mc_is_back_event(event)) {
        app->ui.screen = McScreenTitle;
        app->ui.menu_index = 0U;
    }
}

static void mc_handle_medals_input(McApp* app, const InputEvent* event) {
    if(mc_is_direction_event(event)) {
        app->ui.medal_index = mc_move_index(app->ui.medal_index, McMedalCount, event->key);
    } else if(mc_is_confirm_event(event)) {
        app->ui.screen = McScreenMedalDetails;
    } else if(mc_is_back_event(event)) {
        app->ui.screen = McScreenRecords;
    }
}

static McRunStage mc_stage_for_screen(McScreen screen) {
    if(screen == McScreenWaveResult) return McRunStageWaveResult;
    if(screen == McScreenRepair) return McRunStageRepair;
    return McRunStageActive;
}

static void mc_handle_confirmation_input(McApp* app, const InputEvent* event) {
    if(app->ui.confirm_action == McConfirmClearPace) {
        if(mc_is_direction_event(event))
            app->ui.menu_index ^= 1U;
        else if(mc_is_back_event(event) || mc_is_confirm_event(event)) {
            if(mc_is_confirm_event(event) && app->ui.menu_index) mc_app_clear_pace(app);
            app->ui.screen = app->ui.confirm_return_screen;
            app->ui.menu_index = McSettingsItemClearPace;
            app->ui.confirm_action = McConfirmNone;
        }
        return;
    }
    if(mc_is_back_event(event)) {
        app->ui.screen = app->ui.confirm_return_screen;
        return;
    }
    if(!mc_is_confirm_event(event)) return;

    const McConfirmAction action = app->ui.confirm_action;
    if(action == McConfirmExitUnsaved) {
        app->ui.exit_discard = true;
        app->pending_io = app->failed_io = 0;
        app->settings_dirty = app->scores_dirty = app->profile_dirty = app->run_dirty = false;
        app->ui.screen = McScreenExiting;
    } else if(action == McConfirmOverwriteRun || action == McConfirmRestartRun) {
        if(action == McConfirmRestartRun) {
            app->ui.setup_mode = app->ui.game.mode;
            app->ui.setup_seed = app->ui.game.seed;
            app->ui.setup_options = app->ui.game.options;
            app->ui.retry_seed = true;
        }
        mc_app_start_new_run(app, action == McConfirmOverwriteRun);
    } else if(action == McConfirmAbandonRun) {
        mc_feedback_stop(&app->feedback);
        mc_app_queue_run_delete(app);
        app->ui.screen = McScreenTitle;
        app->ui.menu_index = 0U;
    } else if(action == McConfirmSaveTitle) {
        // Save the underlying screen stage so resume returns to the correct phase
        const McRunStage stage = mc_stage_for_screen(app->ui.confirm_return_screen);
        mc_app_flush_settings(app);
        mc_app_queue_run_save(app, stage, true);
    } else if(action == McConfirmResetSettings) {
        mc_settings_defaults(&app->ui.settings);
        mc_app_refresh_high_score(app);
        mc_app_settings_changed(app);
        mc_app_flush_settings(app);
        app->ui.screen = McScreenSettings;
        app->ui.menu_index = mc_settings_group_item(app->ui.settings_group, 0U);
        mc_feedback_preview_sound(&app->feedback);
        mc_feedback_preview_vibration(&app->feedback);

    } else if(action == McConfirmResetScores) {
        mc_score_tables_defaults(&app->ui.scores);
        mc_app_refresh_high_score(app);
        mc_app_save_scores(app);
        app->ui.screen = app->ui.confirm_return_screen;
    }
    app->ui.confirm_action = McConfirmNone;
}

static void mc_handle_run_setup(McApp* app, const InputEvent* event) {
    const McGameMode previous_mode = app->ui.setup_mode;
    const uint8_t rows = (mc_mode_def(previous_mode)->options != McOptionsNone) ? 5U : 4U;
    const bool fixed_difficulty = mc_mode_def(app->ui.setup_mode)->difficulty < McDifficultyCount;
    if(mc_is_back_event(event)) {
        app->ui.screen = McScreenTitle;
        app->ui.menu_index = 0U;
    } else if(mc_is_direction_event(event)) {
        const bool increment = event->key == InputKeyRight;
        if(event->key == InputKeyUp || event->key == InputKeyDown)
            app->ui.menu_index = mc_move_index(app->ui.menu_index, rows, event->key);
        else if(app->ui.menu_index == 0U)
            app->ui.setup_mode = mc_wrap_step(app->ui.setup_mode, McModeCount, increment);
        else if(app->ui.menu_index == 1U && !fixed_difficulty)
            mc_app_change_difficulty(app, increment);
    } else if(mc_is_confirm_event(event)) {
        if(app->ui.menu_index == 0U)
            app->ui.setup_mode = mc_wrap_increment(app->ui.setup_mode, McModeCount);
        else if(app->ui.menu_index == 1U) {
            if(!fixed_difficulty) mc_app_change_difficulty(app, true);
        } else if(app->ui.menu_index == 2U) {
            if(mc_mode_def(app->ui.setup_mode)->seed_policy == McSeedRandom) {
                // Editing a Classic seed turns it into a reproducible Seeded challenge
                if(app->ui.setup_mode == McModeClassic) app->ui.setup_mode = McModeSeeded;
                app->ui.seed_digit = 0U;
                app->ui.screen = McScreenSeed;
            }
        } else if(app->ui.menu_index == 4U) {
            app->ui.screen = McScreenPracticeSetup;
            app->ui.menu_index = 0U;
        } else if(app->ui.has_suspended_run && mc_mode_has(app->ui.setup_mode, McModeSave))
            mc_app_confirm(app, McConfirmOverwriteRun, McScreenRunSetup);
        else {
            app->ui.duel_player = 0U;
            mc_app_start_new_run(app, true);
        }
    }
    if(mc_mode_def(app->ui.setup_mode)->options == McOptionsNone && app->ui.menu_index > 3U)
        app->ui.menu_index = 3U;
    if(!mc_options_valid(app->ui.setup_mode, &app->ui.setup_options))
        memset(&app->ui.setup_options, 0, sizeof(app->ui.setup_options));
    if(previous_mode != app->ui.setup_mode) app->ui.retry_seed = false;
    app->ui.setup_seed = mc_mode_seed(
        app->ui.setup_mode, app->ui.setup_seed, furi_hal_rtc_get_timestamp(), app->ui.retry_seed);
}

static void mc_handle_seed(McApp* app, const InputEvent* event) {
    if(mc_is_back_event(event) || mc_is_confirm_event(event)) {
        if(app->ui.setup_seed == 0U) app->ui.setup_seed = 1U;
        app->ui.screen = McScreenRunSetup;
        return;
    }
    if(!mc_is_direction_event(event)) return;
    if(event->key == InputKeyLeft)
        app->ui.seed_digit = mc_wrap_decrement(app->ui.seed_digit, 8U);
    else if(event->key == InputKeyRight)
        app->ui.seed_digit = mc_wrap_increment(app->ui.seed_digit, 8U);
    else {
        const uint8_t shift = (7U - app->ui.seed_digit) * 4U;
        // Adding fifteen decrements one hexadecimal digit modulo sixteen
        const uint32_t digit =
            ((app->ui.setup_seed >> shift) + (event->key == InputKeyUp ? 1U : 15U)) & 15U;
        app->ui.setup_seed = (app->ui.setup_seed & ~(15UL << shift)) | (digit << shift);
    }
}

// Prevent inlining the large dispatcher to limit runtime callback register spills
static __attribute__((noinline)) void mc_handle_expansion(McApp* app, const InputEvent* event) {
    const bool direction = mc_is_direction_event(event);
    const bool ok = mc_is_confirm_event(event);
    const bool back = mc_is_back_event(event);
    const bool up = event->key == InputKeyUp || event->key == InputKeyLeft;
    switch(app->ui.screen) {
    case McScreenSettingsGroups:
        if(direction)
            app->ui.menu_index = up ? mc_wrap_decrement(app->ui.menu_index, 4U) :
                                      mc_wrap_increment(app->ui.menu_index, 4U);
        else if(ok) {
            app->ui.settings_group = app->ui.menu_index;
            app->ui.menu_index = app->ui.settings_rows[app->ui.settings_group];
            bool found = false;
            for(uint8_t i = 0U; i < mc_settings_group_count(app->ui.settings_group); i++)
                if(mc_settings_group_item(app->ui.settings_group, i) == app->ui.menu_index)
                    found = true;
            if(!found) app->ui.menu_index = mc_settings_group_item(app->ui.settings_group, 0U);
            app->ui.screen = McScreenSettings;
        } else if(back) {
            app->ui.screen = app->ui.settings_return_screen;
            app->ui.menu_index = 0U;
        }
        break;
    case McScreenPracticeSetup: {
        const bool practice = app->ui.setup_mode == McModePractice;
        const bool puzzle = app->ui.setup_mode == McModePuzzle;
        if(back) {
            app->ui.screen = McScreenRunSetup;
            app->ui.menu_index = 4U;
            break;
        }
        if(!practice && !puzzle) break;
        if(direction && (event->key == InputKeyUp || event->key == InputKeyDown))
            app->ui.menu_index = up ? mc_wrap_decrement(app->ui.menu_index, practice ? 5U : 2U) :
                                      mc_wrap_increment(app->ui.menu_index, practice ? 5U : 2U);
        else if(ok || direction) {
            if(puzzle && app->ui.menu_index == 1U) {
                if(ok) {
                    app->ui.detail_return_screen = McScreenPracticeSetup;
                    app->ui.screen = McScreenPuzzleHint;
                }
            } else if(practice && app->ui.menu_index == 4U) {
                app->ui.settings.practice_aids = !app->ui.settings.practice_aids;
                mc_app_settings_changed(app);
            } else if(practice && app->ui.menu_index == 0U && ok) {
                app->ui.edit_wave = app->ui.setup_options.start_wave + 1U;
                app->ui.seed_digit = 0U;
                app->ui.screen = McScreenWaveEditor;
            } else if(app->ui.menu_index == 0U) {
                uint16_t* wave = &app->ui.setup_options.start_wave;
                const uint16_t max = puzzle ? MC_PUZZLE_COUNT - 1U : UINT16_MAX - 1U;
                *wave = event->key == InputKeyLeft ? (*wave ? *wave - 1U : max) :
                                                     (*wave >= max ? 0U : *wave + 1U);
            } else {
                uint8_t* field = app->ui.menu_index == 1U ? &app->ui.setup_options.enemy :
                                 app->ui.menu_index == 2U ? &app->ui.setup_options.unlimited :
                                                            &app->ui.setup_options.slow;
                const uint8_t choices = app->ui.menu_index == 1U ? 5U : 2U;
                *field = event->key == InputKeyLeft ? mc_wrap_decrement(*field, choices) :
                                                      mc_wrap_increment(*field, choices);
            }
        }
        break;
    }
    case McScreenSlots:
        if(direction)
            app->ui.menu_index = up ? mc_wrap_decrement(app->ui.menu_index, MC_SAVE_SLOTS) :
                                      mc_wrap_increment(app->ui.menu_index, MC_SAVE_SLOTS);
        else if(ok) {
            mc_app_select_slot(app, app->ui.menu_index);
        } else if(back) {
            app->ui.screen = McScreenTitle;
            app->ui.menu_index = 0U;
        }
        break;
    case McScreenStorage:
        if(ok)
            mc_app_retry_storage(app);
        else if(back)
            app->ui.screen = app->ui.detail_return_screen;
        break;
    case McScreenDuelSwap:
        if(ok) {
            app->ui.duel_player = 1U;
            mc_app_retry(app, false);
        } else if(back) {
            app->ui.duel_player = 0U;
            app->ui.screen = McScreenTitle;
        }
        break;
    case McScreenDuelResults:
        if(ok || back) {
            app->ui.duel_player = 0U;
            app->ui.screen = McScreenTitle;
            app->ui.menu_index = 0U;
        }
        break;
    default:
        break;
    }
}

typedef enum {
    McPageNone,
    McPageOk,
    McPageBack,
    McPageHorizontal,
} McPageInput;

// Simple pages share completed gestures; gameplay keeps the original physical events.
static McPageInput mc_page_input(const InputEvent* event) {
    if(mc_is_confirm_event(event)) return McPageOk;
    if(mc_is_back_event(event)) return McPageBack;
    if(mc_is_direction_event(event) && (event->key == InputKeyLeft || event->key == InputKeyRight))
        return McPageHorizontal;
    return McPageNone;
}

// Called only for simple pages by the main dispatcher.
static void mc_handle_simple_page(McUiCommon* ui, const InputEvent* event) {
    const McPageInput input = mc_page_input(event);
    const bool guide = ui->screen == McScreenHudGuide;
    if(input == McPageBack || (input == McPageOk && !guide)) {
        if(ui->screen == McScreenAbout) {
            ui->screen = McScreenTitle;
            ui->menu_index = 0;
        } else
            ui->screen = ui->detail_return_screen;
    } else if(input == McPageHorizontal || (guide && input == McPageOk)) {
        if(guide)
            ui->guide_page ^= 1U;
        else if(ui->screen == McScreenRunStats)
            ui->stats_page ^= 1U;
    }
}

// Keep input routing out of the simulation loop in the combined device build.
static __attribute__((noinline)) void mc_handle_screen_input(McApp* app, const InputEvent* event) {
    if(app->ui.screen == McScreenLoading) return;
    // Edit one-based waves locally; convert to the stored zero-based form only on OK
    if(app->ui.screen == McScreenWaveEditor) {
        if(mc_is_back_event(event) || mc_is_confirm_event(event)) {
            if(mc_is_confirm_event(event))
                app->ui.setup_options.start_wave = app->ui.edit_wave - 1U;
            app->ui.screen = McScreenPracticeSetup;
        } else if(mc_is_direction_event(event)) {
            if(event->key == InputKeyLeft)
                app->ui.seed_digit = mc_wrap_decrement(app->ui.seed_digit, 5);
            else if(event->key == InputKeyRight)
                app->ui.seed_digit = mc_wrap_increment(app->ui.seed_digit, 5);
            else {
                static const uint16_t steps[] = {10000, 1000, 100, 10, 1};
                int32_t value = app->ui.edit_wave + (event->key == InputKeyUp ? 1 : -1) *
                                                        (int32_t)steps[app->ui.seed_digit];
                app->ui.edit_wave = value < 1 ? 1 : value > UINT16_MAX ? UINT16_MAX : value;
            }
        }
        return;
    }

    if(app->ui.screen == McScreenExiting) {
        if(app->ui.exit_discard) return;
        if(mc_is_direction_event(event))
            app->ui.menu_index = (app->ui.menu_index + 1U) % 3U;
        else if(mc_is_back_event(event) || (mc_is_confirm_event(event) && app->ui.menu_index == 1U)) {
            if(atomic_load(&app->worker.state) < McIoStopping) {
                app->ui.exit_pending = app->ui.exit_slow = false;
                app->ui.screen = McScreenTitle;
                app->ui.menu_index = 0;
            }
        } else if(mc_is_confirm_event(event)) {
            if(app->ui.menu_index == 0U)
                mc_app_retry_storage(app);
            else
                mc_app_confirm(app, McConfirmExitUnsaved, McScreenExiting);
        }
        return;
    }
    if(app->ui.screen == McScreenPracticeResult) {
        if(mc_is_direction_event(event))
            app->ui.menu_index ^= 1U;
        else if(mc_is_confirm_event(event) && app->ui.menu_index == 0)
            mc_app_practice_wave(app);
        else if(mc_is_confirm_event(event) || mc_is_back_event(event)) {
            app->ui.screen = McScreenTitle;
            app->ui.menu_index = 0;
        }
        return;
    }
    switch(app->ui.screen) {
    case McScreenCleanup:
        if(app->ui.cleanup_confirm) {
            if(mc_is_back_event(event) || mc_is_long_back_event(event)) {
                app->ui.cleanup_confirm = false;
                app->ui.menu_index = 0;
            } else if(mc_is_direction_event(event)) {
                app->ui.menu_index ^= 1U;
            } else if(mc_is_confirm_event(event)) {
                if(app->ui.menu_index) app->cleanup_action = 2U;
                app->ui.cleanup_confirm = false;
                app->ui.menu_index = 0;
            }
            break;
        }
        if(app->ui.cleanup_state == McCleanupScanning ||
           app->ui.cleanup_state == McCleanupPurging ||
           app->ui.cleanup_state == McCleanupMigrating ||
           app->ui.cleanup_state == McCleanupValidating)
            break;
        if(mc_is_back_event(event) || mc_is_long_back_event(event))
            app->cleanup_action = 1U;
        else if(mc_is_direction_event(event)) {
            if(event->key == InputKeyUp || event->key == InputKeyDown)
                mc_app_cleanup_select(app, event->key == InputKeyDown);
            else
                app->ui.menu_index = mc_wrap_step(
                    app->ui.menu_index,
                    app->ui.cleanup_can_migrate && app->ui.cleanup_state == McCleanupPrompt ? 3U :
                                                                                              2U,
                    event->key == InputKeyRight);
        } else if(mc_is_confirm_event(event)) {
            if(!app->ui.menu_index)
                app->cleanup_action = 1U;
            else if(app->ui.cleanup_state == McCleanupFailed)
                app->cleanup_action = 3U;
            else if(app->ui.cleanup_can_migrate && app->ui.menu_index == 1U)
                app->cleanup_action = 4U;
            else {
                app->ui.cleanup_confirm = true;
                app->ui.menu_index = 0;
            }
        }
        break;
    case McScreenPracticeSetup:
    case McScreenSettingsGroups:
    case McScreenStorage:
    case McScreenSlots:
    case McScreenDuelSwap:
    case McScreenDuelResults:
        mc_handle_expansion(app, event);
        break;
    case McScreenRunSetup:
        mc_handle_run_setup(app, event);
        break;
    case McScreenSeed:
        mc_handle_seed(app, event);
        break;
    case McScreenLesson:
        if(mc_is_confirm_event(event))
            app->ui.screen = McScreenPlaying;
        else if(mc_is_back_event(event)) {
            app->ui.screen = McScreenPaused;
            app->ui.menu_index = 0U;
        }
        break;
    case McScreenModeRecords:
        if(mc_is_back_event(event)) {
            app->ui.screen = McScreenRecords;
            app->ui.record_index = 0U;
        } else if(mc_is_direction_event(event)) {
            if(event->key == InputKeyLeft || event->key == InputKeyRight) {
                app->ui.screen = McScreenRecords;
                app->ui.record_index = 0U;
            } else if(event->key == InputKeyUp)
                // Training follows scored modes in the enum and has no mode leaderboard
                app->ui.record_index = mc_wrap_decrement(app->ui.record_index, McModeCount);
            else if(event->key == InputKeyDown)
                app->ui.record_index = mc_wrap_increment(app->ui.record_index, McModeCount);
        }
        break;
    case McScreenTitle:
        mc_handle_title_input(app, event);
        break;
    case McScreenControlCard:
        mc_handle_control_card_input(app, event);
        break;
    case McScreenPlaying:
        mc_handle_playing_input(app, event);
        break;
    case McScreenPaused:
        mc_handle_paused_input(app, event);
        break;
    case McScreenWaveResult:
        mc_handle_wave_result_input(app, event);
        break;
    case McScreenRepair:
        mc_handle_repair_input(app, event);
        break;
    case McScreenGameOver:
        mc_handle_game_over_input(app, event);
        break;
    case McScreenHighScores:
        mc_handle_scores_input(app, event);
        break;
    case McScreenRecords:
        mc_handle_records_input(app, event);
        break;
    case McScreenMedals:
        mc_handle_medals_input(app, event);
        break;
    case McScreenMedalDetails:
        if(mc_is_confirm_event(event)) {
            app->ui.profile.pinned_medal = app->ui.profile.pinned_medal == app->ui.medal_index ?
                                               UINT8_MAX :
                                               app->ui.medal_index;
            mc_app_profile_changed(app);
        } else if(mc_is_back_event(event)) {
            app->ui.screen = McScreenMedals;
        }
        break;
    case McScreenSettings:
        mc_handle_settings_input(app, event);
        break;
    case McScreenPuzzleHint:
    case McScreenScoreDetails:
    case McScreenRunStats:
    case McScreenAbout:
    case McScreenHudGuide:
        mc_handle_simple_page(&app->ui.common, event);
        break;
    case McScreenConfirm:
        mc_handle_confirmation_input(app, event);
        break;
    }
}

void mc_app_handle_input(McApp* app, const InputEvent* event) {
    const McScreen previous_screen = app->ui.screen;
    mc_handle_screen_input(app, event);
    // Every entry into play, including cancelled dialogs and new waves, gets ready time.
    if(previous_screen != McScreenPlaying && app->ui.screen == McScreenPlaying)
        mc_app_continue(app);
    mc_help_refresh(&app->ui.common, &app->ui.game);
}
