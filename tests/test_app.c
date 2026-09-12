// Core application integration checks against deterministic virtual storage and input
#include "host_platform.h"
#include "app_internal.h"
#include "collision.h"
#include "render.h"
#include "runtime.h"
#include "help_storage.h"
#include "binary.h"
#include "ui_menu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Compile the real loop and I/O coordinator against a virtual clock and storage */
#include "../src/app.c"

// Complete immediately-ready worker requests without advancing virtual time.
static bool test_io(McApp* app, uint32_t now) {
    bool changed = false;
    for(unsigned i = 0; i < 256U; i++) {
        changed |= mc_app_process_io(app, now);
        assert(!app->cleanup.allocated || !app->persistence.history);
        if(atomic_load(&app->worker.state) != McIoComplete) break;
    }
    return changed;
}

// Finish bounded startup and cleanup passes before returning an app ready for menu input
static McApp* test_app_alloc(void) {
    McApp* app = mc_app_alloc();
    for(unsigned i = 0U; i < 100U && (app->startup_step || app->ui.screen == McScreenCleanup); i++)
        test_io(app, 0U);
    assert(app->ui.screen == McScreenTitle);
    return app;
}

// Verify recoverable writes, backup preservation, and retryable checkpoint deletion
static void test_storage_transactions(void) {
    // Fail each write stage; reload must recover the old value or a published new one
    for(int fault = 0; fault <= 5; fault++) {
        host_reset();
        McPersistence p;
        Storage* storage = furi_record_open(RECORD_STORAGE);
        assert(mc_persistence_init(&p, storage));
        McSettings settings;
        mc_settings_defaults(&settings);
        settings.cursor_speed = McCursorSlow;
        assert(mc_persistence_save_settings(&p, &settings) == McStorageOk);
        settings.cursor_speed = McCursorNormal;
        assert(mc_persistence_save_settings(&p, &settings) == McStorageOk);
        settings.cursor_speed = McCursorFast;
        host_fail_storage_after(fault);
        McStorageResult write = mc_persistence_save_settings(&p, &settings);
        mc_persistence_deinit(&p);
        host_fail_storage_after(-1);
        assert(mc_persistence_init(&p, storage));
        assert(mc_persistence_load_settings(&p, &settings) == McStorageOk);
        assert(settings.cursor_speed == (write == McStorageOk ? McCursorFast : McCursorNormal));
        mc_persistence_deinit(&p);
    }
    host_reset();
    McPersistence p;
    assert(mc_persistence_init(&p, furi_record_open(RECORD_STORAGE)));
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 44U);

    McRunSnapshot run = {.game = &game, .stage = McRunStageActive};
    assert(mc_persistence_checkpoint(&p, &run) == McStorageOk);
    const uint32_t hash = mc_game_state_hash(&game);
    mc_game_step(&game, NULL);
    assert(mc_persistence_checkpoint(&p, &run) == McStorageOk);
    host_corrupt(APP_DATA_PATH(MC_DATA_FOLDER "/save0.dat"));
    assert(mc_persistence_restore(&p, &run) == McStorageOk && p.recovered);
    assert(mc_game_state_hash(&game) == hash);
    host_fail_storage_after(3); /* Failure publishing recovered data preserves backup */
    assert(mc_persistence_checkpoint(&p, &run) == McStorageIoError);
    host_fail_storage_after(-1);
    assert(mc_persistence_restore(&p, &run) == McStorageOk && p.recovered);
    assert(mc_game_state_hash(&game) == hash);
    assert(mc_persistence_checkpoint(&p, &run) == McStorageOk);
    assert(mc_persistence_remove_checkpoint(&p) == McStorageOk);
    assert(mc_persistence_restore(&p, &run) == McStorageMissing);
    for(int fault = 0; fault < 2; fault++) {
        assert(mc_persistence_checkpoint(&p, &run) == McStorageOk);
        assert(mc_persistence_checkpoint(&p, &run) == McStorageOk);
        host_fail_storage_after(fault);
        assert(mc_persistence_remove_checkpoint(&p) == McStorageIoError);
        host_fail_storage_after(-1);
        assert(mc_persistence_remove_checkpoint(&p) == McStorageOk);
        assert(mc_persistence_restore(&p, &run) == McStorageMissing);
    }
    mc_persistence_deinit(&p);
}

// Deliver one gesture directly to the real screen controller
static void send(McApp* app, InputKey key, InputType type) {
    const InputEvent event = {.key = key, .type = type};
    mc_app_handle_input(app, &event);
}

// Ready time freezes the entire game and ignores play input without restarting the timer.
static void finish_countdown(McApp* app) {
    assert(app->ui.screen == McScreenPlaying);
    const unsigned ticks = app->ui.settings.resume_countdown ? 90U : 0U;
    assert(app->ui.countdown_ticks == ticks);
    assert(!app->held_directions && !app->cursor_hold_ticks);
    const McGame before = app->ui.game;
    for(unsigned i = 0; i < ticks; i++) {
        send(app, InputKeyOk, InputTypeShort);
        send(app, InputKeyRight, InputTypePress);
        send(app, InputKeyRight, InputTypeRelease);
        mc_app_step(app);
        assert(app->ui.countdown_ticks == ticks - i - 1U);
    }
    assert(!memcmp(&before, &app->ui.game, sizeof(before)));
}

static void test_countdown_play_transitions(void) {
    for(unsigned enabled = 0; enabled < 2; enabled++) {
        host_reset();
        McApp* app = test_app_alloc();
        assert(app->ui.settings.resume_countdown);
        app->ui.settings.resume_countdown = enabled;
        app->ui.setup_mode = McModeSeeded;
        app->ui.setup_seed = 1234U;

        // A control card delays the countdown until the player starts the round.
        mc_app_start_new_run(app, true);
        assert(app->ui.screen == McScreenControlCard && !app->ui.countdown_ticks);
        send(app, InputKeyOk, InputTypeShort);
        finish_countdown(app);
        mc_app_step(app);
        assert(app->ui.game.stats.play_ticks == 1U);
        send(app, InputKeyOk, InputTypeShort);
        assert(app->ui.game.stats.shots_fired == 1U);
        test_io(app, 0);

        // Both pause gestures and cancelling Save & Title must restart ready time.
        send(app, InputKeyBack, InputTypeShort);
        assert(app->ui.screen == McScreenPaused);
        send(app, InputKeyOk, InputTypeShort);
        mc_app_step(app);
        send(app, InputKeyBack, InputTypeShort);
        assert(app->ui.screen == McScreenPaused && !app->ui.countdown_ticks);
        send(app, InputKeyBack, InputTypeShort);
        finish_countdown(app);
        send(app, InputKeyBack, InputTypeLong);
        assert(app->ui.screen == McScreenConfirm);
        send(app, InputKeyBack, InputTypeShort);
        finish_countdown(app);
        test_io(app, 0);

        // Restoring an active save opens Pause; ready time starts on Resume.
        mc_app_resume_run(app);
        test_io(app, 0);
        assert(app->ui.screen == McScreenPaused);
        send(app, InputKeyBack, InputTypeShort);
        finish_countdown(app);

        // Cover next waves reached directly and through the workshop.
        for(unsigned workshop = 0; workshop < 2; workshop++) {
            app->ui.game.phase = McGamePhaseWaveResult;
            app->ui.game.repair_credits = workshop;
            app->ui.screen = McScreenWaveResult;
            const uint16_t wave = app->ui.game.wave;
            send(app, InputKeyOk, InputTypeShort);
            if(workshop) {
                assert(app->ui.screen == McScreenRepair);
                send(app, InputKeyBack, InputTypeShort);
            }
            assert(app->ui.game.wave == wave + 1U);
            finish_countdown(app);
            test_io(app, 0);
        }

        // Direct starts, retries, and exact wave practice use the same countdown.
        app->ui.settings.show_control_card = false;
        mc_app_start_new_run(app, true);
        finish_countdown(app);
        test_io(app, 0);
        mc_app_retry(app, false);
        finish_countdown(app);
        test_io(app, 0);
        mc_app_practice_wave(app);
        assert(app->ui.wave_practice);
        finish_countdown(app);
        mc_app_retry(app, false);
        finish_countdown(app);

        // Training starts ready time after the lesson, including subsequent lessons.
        app->ui.setup_mode = McModeTraining;
        mc_app_start_new_run(app, false);
        assert(app->ui.screen == McScreenLesson && !app->ui.countdown_ticks);
        send(app, InputKeyOk, InputTypeShort);
        finish_countdown(app);
        app->ui.game.phase = McGamePhaseWaveResult;
        app->ui.screen = McScreenWaveResult;
        send(app, InputKeyOk, InputTypeShort);
        assert(app->ui.screen == McScreenLesson && !app->ui.countdown_ticks);
        send(app, InputKeyOk, InputTypeShort);
        finish_countdown(app);
        mc_app_free(app);
    }
}

// Drain pace maintenance within a fixed step bound and require successful completion
static void history(McPersistence* p) {
    McStorageResult r = McStorageBusy;
    for(unsigned i = 0; i < 4096 && r == McStorageBusy; i++)
        r = mc_persistence_history_step(p);
    assert(r == McStorageOk);
}
// Check the canonical pace filename for the fixture's mode, seed, and assigned difficulty
static bool exists(McPersistence* p, unsigned mode, unsigned seed) {
    char path[96];
    snprintf(
        path,
        sizeof(path),
        APP_DATA_PATH(MC_DATA_FOLDER "/pace-2-%u-%u-0-%08x.dat"),
        mode,
        seed % 3,
        seed);
    return storage_file_exists(p->storage, path);
}
// Write a comparison fixture across difficulties through the production persistence path
static void save_pace(McPersistence* p, unsigned mode, unsigned seed, unsigned score) {
    McGame g;
    mc_game_start_mode(&g, seed % 3, seed, mode);
    // Daily fixes difficulty to Command
    g.difficulty = seed % 3;
    McPace pace = {.score = score};
    assert(mc_persistence_pace(p, &g, &pace, false) != McStorageIoError);
    pace.score = score;
    assert(mc_persistence_pace(p, &g, &pace, true) == McStorageOk);
}
// Verify highest-score import, save-order eviction, publication failure, and resumable clearing
static void test_retention(void) {
    host_reset();
    McPersistence p;
    assert(mc_persistence_init(&p, furi_record_open(RECORD_STORAGE)));
    for(unsigned m = McModeDaily; m <= McModeSeeded; m++)
        for(unsigned seed = 1; seed <= 20; seed++)
            save_pace(&p, m, seed, seed * 100);
    mc_persistence_deinit(&p); // Existing, unindexed files represent legacy history
    assert(mc_persistence_init(&p, furi_record_open(RECORD_STORAGE)));
    history(&p);
    for(unsigned m = McModeDaily; m <= McModeSeeded; m++)
        for(unsigned seed = 1; seed <= 20; seed++)
            assert(exists(&p, m, seed) == (seed >= 5));
    // Resaving an old identity makes it newest before a low-scoring new save evicts the oldest
    save_pace(&p, McModeSeeded, 5, 5000);
    history(&p);
    save_pace(&p, McModeSeeded, 21, 1);
    // An index publication failure must preserve both old and new pace files until retry succeeds
    host_fail_next(HostStorageWrite);
    assert(mc_persistence_history_step(&p) == McStorageIoError);
    assert(exists(&p, McModeSeeded, 6) && exists(&p, McModeSeeded, 21));
    history(&p);
    assert(
        exists(&p, McModeSeeded, 5) && !exists(&p, McModeSeeded, 6) &&
        exists(&p, McModeSeeded, 21));
    assert(exists(&p, McModeDaily, 6));
    save_pace(&p, McModeClassic, 0, 100);
    history(&p);
    File* f = storage_file_alloc(p.storage);
    assert(storage_file_open(
        f, APP_DATA_PATH(MC_DATA_FOLDER "/pace-unrelated.dat"), FSAM_WRITE, FSOM_CREATE_ALWAYS));
    storage_file_close(f);
    storage_file_free(f);
    mc_persistence_history_clear(&p);
    // Persist clear intent, then restart halfway through the operation
    assert(mc_persistence_history_step(&p) == McStorageBusy);
    mc_persistence_deinit(&p);
    assert(mc_persistence_init(&p, furi_record_open(RECORD_STORAGE)));
    history(&p);
    for(unsigned m = McModeDaily; m <= McModeSeeded; m++)
        for(unsigned seed = 1; seed <= 21; seed++)
            assert(!exists(&p, m, seed));
    assert(storage_file_exists(p.storage, APP_DATA_PATH(MC_DATA_FOLDER "/pace-unrelated.dat")));
    assert(!exists(&p, McModeClassic, 0));
    mc_persistence_deinit(&p);
}

// Check battery gestures, exact run restoration, workshop restoration, and deletion retry
static void test_ui_and_save_resume(void) {
    host_reset();
    McApp* app = test_app_alloc();
    for(unsigned i = 0U; i < 100U && (app->startup_step || app->ui.screen == McScreenCleanup); i++)
        test_io(app, 0U);
    send(app, InputKeyOk, InputTypeShort);
    assert(app->ui.screen == McScreenRunSetup);
    app->ui.setup_mode = McModeTraining;
    app->ui.menu_index = 3U;
    send(app, InputKeyOk, InputTypeShort);
    assert(app->ui.screen == McScreenLesson);
    send(app, InputKeyOk, InputTypeShort);
    assert(app->ui.screen == McScreenPlaying);
    finish_countdown(app);
    const uint32_t shots = app->ui.game.stats.shots_fired;
    send(app, InputKeyOk, InputTypeLong);
    send(app, InputKeyRight, InputTypePress);
    send(app, InputKeyOk, InputTypeRelease);
    assert(!app->ui.battery_overlay && app->ui.game.battery_selection == McBatteryRight);
    assert(app->ui.game.stats.shots_fired == shots);
    send(app, InputKeyOk, InputTypeShort);
    assert(app->ui.game.stats.shots_fired == shots + 1U);
    const McGame before_selection = app->ui.game;
    app->ui.game.alive_site_mask &= (uint16_t)~1U;
    app->ui.game.battery_ammo[0] = 0U;
    send(app, InputKeyOk, InputTypeLong);
    send(app, InputKeyLeft, InputTypePress);
    assert(app->ui.game.battery_selection == McBatteryRight);
    send(app, InputKeyDown, InputTypePress);
    assert(app->ui.game.battery_selection == McBatteryAuto);
    send(app, InputKeyOk, InputTypeRelease);
    assert(!app->ui.battery_overlay && app->ui.game.stats.shots_fired == shots + 1U);
    app->ui.game = before_selection;
    test_io(app, furi_get_tick());
    app->ui.setup_mode = McModeSeeded;
    app->ui.setup_seed = 99U;
    mc_app_start_new_run(app, false);
    test_io(app, furi_get_tick());
    finish_countdown(app);
    send(app, InputKeyOk, InputTypeShort);
    for(uint8_t i = 0U; i < 40U; i++)
        mc_app_step(app);
    send(app, InputKeyBack, InputTypeShort);
    test_io(app, furi_get_tick());
    const uint32_t hash = mc_game_state_hash(&app->ui.game);
    mc_game_init(&app->ui.game);
    mc_app_resume_run(app);
    test_io(app, furi_get_tick());
    assert(app->ui.screen == McScreenPaused);
    assert(mc_game_state_hash(&app->ui.game) == hash);
    app->ui.game.phase = McGamePhaseWaveResult;
    app->ui.game.alive_site_mask &= (uint16_t) ~(1U << 4U);
    mc_app_queue_run_save(app, McRunStageRepair, false);
    test_io(app, furi_get_tick());
    mc_app_resume_run(app);
    test_io(app, furi_get_tick());
    assert(app->ui.screen == McScreenRepair && app->ui.repair_site_index == 4U);
    assert(app->ui.supply_index == 0U);
    mc_app_queue_run_delete(app);
    host_fail_storage_after(0);
    test_io(app, furi_get_tick());
    assert(app->failed_io & McPendingIoRunDelete);
    host_fail_storage_after(-1);
    test_io(app, app->retry_due_tick);
    assert(!(app->failed_io & McPendingIoRunDelete));
    mc_app_free(app);
}

// Compare rendering modes and verify paused time and stalls cannot replay extra simulation
static void test_real_application_loop(void) {
    // Enter a run, hold movement, pause, resume after a long gap, then leave through the save flow
    HostInput script[] = {
        {0U, InputKeyOk, InputTypeShort},       {1U, InputKeyDown, InputTypePress},
        {2U, InputKeyDown, InputTypePress},     {3U, InputKeyDown, InputTypePress},
        {4U, InputKeyOk, InputTypeShort},       {5U, InputKeyOk, InputTypeShort},
        {6U, InputKeyRight, InputTypePress},    {7U, InputKeyRight, InputTypeRepeat},
        {8U, InputKeyRight, InputTypeRepeat},   {9U, InputKeyRight, InputTypeRepeat},
        {10U, InputKeyRight, InputTypeRepeat},  {11U, InputKeyRight, InputTypeRepeat},
        {12U, InputKeyRight, InputTypeRepeat},  {800U, InputKeyRight, InputTypeRelease},
        {1005U, InputKeyBack, InputTypeShort},  {10005U, InputKeyBack, InputTypeShort},
        {11005U, InputKeyBack, InputTypeShort}, {11006U, InputKeyBack, InputTypeLong},
        {11007U, InputKeyOk, InputTypeShort},   {11008U, InputKeyBack, InputTypeShort},
    };
    // Leave startup time and three seconds for ready time at each entry into play.
    for(size_t i = 0U; i < sizeof(script) / sizeof(script[0]); i++) {
        const uint32_t tick = script[i].tick;
        script[i].tick += 100U + (tick >= 6U ? 3000U : 0U) + (tick >= 11005U ? 3000U : 0U);
    }
    uint32_t redraws[2];
    for(uint8_t mode = 0; mode < McRenderModeCount; mode++) {
        host_reset();
        McPersistence p;
        assert(mc_persistence_init(&p, furi_record_open(RECORD_STORAGE)));
        McSettings settings;
        mc_settings_defaults(&settings);
        settings.render_mode = mode;
        assert(mc_persistence_save_settings(&p, &settings) == McStorageOk);
        mc_persistence_deinit(&p);
        host_inputs(script, sizeof(script) / sizeof(script[0]), 1000U);
        assert(mc_app_run() == 0);
        assert(host_pause_count == 2U);
        // Both render modes must reach the same simulation ticks on either side of the pause
        assert(host_sim_at_pause[0] == 29U);
        assert(host_sim_at_pause[1] == 58U);
        assert(host_final_ui.has_suspended_run && host_final_ui.game.stats.play_ticks == 58U);
        redraws[mode] = host_redraws;
    }
    assert(redraws[McRenderBatterySaver] < redraws[McRenderAdaptive]);
    host_reset();
    host_inputs(script, sizeof(script) / sizeof(script[0]), 1000U);
    host_stall_at(205U, 500U);
    assert(mc_app_run() == 0);
    assert(host_sim_at_pause[0] >= 15U && host_sim_at_pause[0] <= 18U);
    assert(host_sim_at_pause[1] - host_sim_at_pause[0] == 29U);
}

// Verify grouped navigation remembers selection and the Practice editor cancels and clamps
static void test_settings_and_setup(void) {
    host_reset();
    McApp* app = test_app_alloc();
    mc_app_open_settings(app, McScreenTitle);
    for(uint8_t group = 0U; group < 4U; group++) {
        assert(app->ui.screen == McScreenSettingsGroups && app->ui.menu_index == group);
        send(app, InputKeyOk, InputTypeShort);
        assert(app->ui.screen == McScreenSettings && app->ui.settings_group == group);
        assert(app->ui.menu_index == mc_settings_group_item(group, 0U));
        send(app, InputKeyDown, InputTypePress);
        const uint8_t selected = app->ui.menu_index;
        assert(selected == mc_settings_group_item(group, 1U));
        send(app, InputKeyBack, InputTypeShort);
        assert(app->ui.screen == McScreenSettingsGroups && app->ui.menu_index == group);
        send(app, InputKeyOk, InputTypeShort);
        assert(app->ui.screen == McScreenSettings && app->ui.menu_index == selected);
        send(app, InputKeyBack, InputTypeShort);
        send(app, InputKeyDown, InputTypePress);
    }
    send(app, InputKeyBack, InputTypeShort);
    assert(app->ui.screen == McScreenTitle);
    mc_app_open_run_setup(app);
    for(unsigned i = 0; i < 4; i++)
        send(app, InputKeyDown, InputTypePress);
    assert(app->ui.menu_index == 0); // Classic has no mode-options row
    app->ui.setup_mode = McModePractice;
    app->ui.menu_index = 4;
    send(app, InputKeyOk, InputTypeShort);
    send(app, InputKeyOk, InputTypeShort);
    assert(app->ui.screen == McScreenWaveEditor && app->ui.edit_wave == 1);
    send(app, InputKeyUp, InputTypePress);
    assert(app->ui.edit_wave == 10001);
    send(app, InputKeyBack, InputTypeShort);
    assert(app->ui.setup_options.start_wave == 0);
    send(app, InputKeyOk, InputTypeShort);
    app->ui.edit_wave = 65000;
    send(app, InputKeyUp, InputTypePress);
    assert(app->ui.edit_wave == 65535);
    send(app, InputKeyOk, InputTypeShort);
    assert(app->ui.setup_options.start_wave == 65534);
    mc_app_free(app);
}

static void callback(McApp* app, InputKey key, InputType type, uint32_t sequence) {
    InputEvent event = {.key = key, .type = type, .sequence = sequence};
    mc_view_input_callback(&event, app);
}
static void test_input_fifo_and_overflow(void) {
    host_reset();
    McApp* app = test_app_alloc();
    test_io(app, 0);
    callback(app, InputKeyOk, InputTypePress, 1);
    callback(app, InputKeyOk, InputTypeRelease, 2);
    callback(app, InputKeyOk, InputTypeShort, 3);
    InputEvent event;
    assert(mc_app_get_input(app, &event, 0) == FuriStatusOk && event.sequence == 1);
    callback(app, InputKeyBack, InputTypePress, 4);
    for(unsigned n = 2; n <= 4; n++)
        assert(mc_app_get_input(app, &event, 0) == FuriStatusOk && event.sequence == n);
    callback(app, InputKeyBack, InputTypeRelease, 5);
    assert(mc_app_get_input(app, &event, 0) == FuriStatusOk);
    for(unsigned i = 0; i < 40; i++)
        callback(app, InputKeyLeft, InputTypeRepeat, i);
    assert(furi_message_queue_get_space(app->input_queue) == 8);
    callback(app, InputKeyOk, InputTypePress, 100);
    callback(app, InputKeyOk, InputTypeLong, 101);
    callback(app, InputKeyOk, InputTypeRelease, 102);
    for(unsigned i = 0; i < 24; i++)
        assert(mc_app_get_input(app, &event, 0) == FuriStatusOk && event.sequence == i);
    for(unsigned i = 100; i <= 102; i++)
        assert(mc_app_get_input(app, &event, 0) == FuriStatusOk && event.sequence == i);
    mc_game_start_run(&app->ui.game, McDifficultyCommand, 1);
    app->ui.screen = McScreenPlaying;
    app->ui.battery_overlay = true;
    for(unsigned i = 0; i < 33; i++)
        callback(app, InputKeyLeft, InputTypePress, i);
    assert(mc_app_get_input(app, &event, 0) == FuriStatusErrorTimeout);
    assert(app->ui.screen == McScreenPaused && !app->ui.battery_overlay && !app->held_directions);
    callback(app, InputKeyLeft, InputTypeRelease, 1);
    mc_app_get_input(app, &event, 0);
    callback(app, InputKeyLeft, InputTypeShort, 2); // Completion belonging to discarded burst
    assert(mc_app_get_input(app, &event, 0) != FuriStatusOk);
    callback(app, InputKeyOk, InputTypePress, 3);
    callback(app, InputKeyOk, InputTypeShort, 4);
    assert(mc_app_get_input(app, &event, 0) == FuriStatusOk && event.sequence == 3);
    assert(mc_app_get_input(app, &event, 0) == FuriStatusOk && event.sequence == 4);
    mc_app_free(app);
}

static void drain_input(McApp* app) {
    InputEvent event;
    while(mc_app_get_input(app, &event, 0) == FuriStatusOk)
        mc_app_handle_input(app, &event);
}
static void test_callback_battery_gestures(void) {
    host_reset();
    McApp* app = test_app_alloc();
    test_io(app, 0);
    mc_game_start_run(&app->ui.game, McDifficultyCommand, 42);
    app->ui.screen = McScreenPlaying;
    const uint32_t shots = app->ui.game.stats.shots_fired;
    callback(app, InputKeyOk, InputTypePress, 1);
    callback(app, InputKeyOk, InputTypeLong, 2);
    callback(app, InputKeyRight, InputTypePress, 3);
    callback(app, InputKeyRight, InputTypeShort, 4);
    callback(app, InputKeyRight, InputTypeRelease, 5);
    callback(app, InputKeyOk, InputTypeRelease, 6);
    drain_input(app);
    assert(!app->ui.battery_overlay && app->ui.game.battery_selection == McBatteryRight);
    assert(app->ui.game.stats.shots_fired == shots);
    callback(app, InputKeyOk, InputTypePress, 7);
    callback(app, InputKeyOk, InputTypeRelease, 8);
    callback(app, InputKeyOk, InputTypeShort, 9);
    drain_input(app);
    assert(app->ui.game.stats.shots_fired == shots + 1U);
    // A release rejected by a full queue still updates physical state and pauses safely.
    callback(app, InputKeyLeft, InputTypePress, 10);
    drain_input(app);
    assert(app->held_directions);
    for(unsigned n = 0; n < MC_INPUT_QUEUE_CAPACITY; n++)
        callback(app, InputKeyBack, InputTypeRelease, n);
    callback(app, InputKeyLeft, InputTypeRelease, 11);
    drain_input(app);
    assert(!app->held_directions && app->ui.screen == McScreenPaused);
    assert(!atomic_load(&app->physical_keys));
    mc_app_free(app);
}

static McApp* render_app;
static void mutate_after_snapshot(void) {
    furi_mutex_acquire(render_app->mutex, FuriWaitForever);
    render_app->ui.game.score += 1234;
    mc_app_refresh_render_cache(render_app);
    furi_mutex_release(render_app->mutex);
}
static void test_render_snapshot_consistency(void) {
    host_reset();
    McApp* app = test_app_alloc();
    test_io(app, 0);
    mc_game_start_run(&app->ui.game, McDifficultyCommand, 42);
    app->ui.screen = McScreenPlaying;
    mc_app_refresh_render_cache(app);
    Canvas expected = {0}, actual = {0};
    McRenderSnapshot before;
    mc_render_snapshot(&before, &app->ui);
    mc_render(&expected, &before);
    render_app = app;
    host_render_hook = mutate_after_snapshot;
    mc_view_draw_callback(&actual, app);
    assert(app->ui.game.score == before.game.score + 1234);
    assert(!memcmp(expected.pixels, actual.pixels, sizeof(expected.pixels)));
    mc_app_free(app);
}

static void test_inverted_frames_and_overlays(void) {
    host_reset();
    McApp* app = test_app_alloc();
    test_io(app, 0);
    mc_game_start_run(&app->ui.game, McDifficultyCommand, 42);
    mc_game_fire(&app->ui.game);
    app->ui.storage_pending = app->ui.storage_notice_visible = true;
    app->ui.storage_result = McStorageBusy;
    app->ui.storage_item = "Settings";
    app->ui.battery_overlay = true;
    const McScreen screens[] = {
        McScreenTitle,
        McScreenPlaying,
        McScreenPaused,
        McScreenSettingsGroups,
        McScreenStorage,
        McScreenExiting};
    for(unsigned i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        app->ui.screen = screens[i];
        app->ui.exit_pending = screens[i] == McScreenExiting;
        app->ui.settings.invert_colors = false;
        mc_app_refresh_render_cache(app);
        Canvas normal = {0}, inverted = {0};
        mc_view_draw_callback(&normal, app);
        app->ui.settings.invert_colors = true;
        // Redrawing must not accumulate inversion or omit notices and battery selection.
        for(unsigned frame = 0; frame < 2; frame++) {
            mc_view_draw_callback(&inverted, app);
            for(unsigned y = 0; y < 64; y++)
                for(unsigned x = 0; x < 128; x++)
                    assert(inverted.pixels[y][x] == (normal.pixels[y][x] ^ 1U));
        }
        app->ui.settings.invert_colors = false;
        mc_view_draw_callback(&inverted, app);
        assert(!memcmp(normal.pixels, inverted.pixels, sizeof(normal.pixels)));
    }
    mc_app_free(app);
}

static void test_invert_setting_survives_restart(void) {
    host_reset();
    McApp* app = test_app_alloc();
    assert(!app->ui.settings.invert_colors);
    for(unsigned pass = 0; pass < 2; pass++) {
        mc_app_open_settings(app, McScreenTitle);
        send(app, InputKeyDown, InputTypePress);
        send(app, InputKeyOk, InputTypeShort);
        assert(app->ui.settings_group == 1U);
        for(unsigned row = 0; app->ui.menu_index != McSettingsItemInvertColors; row++) {
            assert(row < mc_settings_group_count(1U));
            send(app, InputKeyDown, InputTypePress);
        }
        send(app, pass ? InputKeyLeft : InputKeyOk, pass ? InputTypePress : InputTypeShort);
        assert(app->ui.settings.invert_colors == !pass && app->settings_dirty);
        assert(!strcmp(
            mc_setting_value(&app->ui.common, McSettingsItemInvertColors), pass ? "Off" : "On"));
        send(app, InputKeyBack, InputTypeShort);
        test_io(app, furi_get_tick());
        assert(!app->settings_dirty);
        mc_app_free(app);
        app = test_app_alloc();
        assert(app->ui.settings.invert_colors == !pass);
    }
    mc_app_free(app);
}

static void test_resume_timer_setting_survives_restart(void) {
    host_reset();
    McApp* app = test_app_alloc();
    assert(app->ui.settings.resume_countdown);
    for(unsigned pass = 0; pass < 2; pass++) {
        mc_app_open_settings(app, McScreenTitle);
        send(app, InputKeyOk, InputTypeShort);
        assert(app->ui.settings_group == 0U);
        for(unsigned row = 0; app->ui.menu_index != McSettingsItemCountdown; row++) {
            assert(row < mc_settings_group_count(0U));
            send(app, InputKeyDown, InputTypePress);
        }
        assert(!strcmp(mc_setting_label(McSettingsItemCountdown), "Resume timer"));
        send(app, pass ? InputKeyRight : InputKeyOk, pass ? InputTypePress : InputTypeShort);
        assert(app->ui.settings.resume_countdown == (bool)pass && app->settings_dirty);
        assert(!strcmp(
            mc_setting_value(&app->ui.common, McSettingsItemCountdown), pass ? "On" : "Off"));
        send(app, InputKeyBack, InputTypeShort);
        test_io(app, 0);
        assert(!app->settings_dirty);
        mc_app_free(app);
        app = test_app_alloc();
        assert(app->ui.settings.resume_countdown == (bool)pass);
    }
    app->ui.settings.resume_countdown = false;
    mc_app_confirm(app, McConfirmResetSettings, McScreenSettings);
    send(app, InputKeyOk, InputTypeShort);
    assert(app->ui.settings.resume_countdown);
    mc_app_free(app);
}

static void test_worker_edits_and_exit(void) {
    host_reset();
    McApp* app = test_app_alloc();
    test_io(app, 0);
    app->ui.settings.sound = false;
    mc_app_settings_changed(app);
    mc_app_flush_settings(app);
    host_defer_io(true);
    mc_app_process_io(app, 0);
    assert(atomic_load(&app->worker.state) == McIoPending);
    app->ui.settings.sound = true;
    mc_app_settings_changed(app);
    host_complete_io();
    mc_app_process_io(app, 0);
    assert(!app->saved_settings.sound && app->ui.settings.sound && app->settings_dirty);
    host_defer_io(false);
    mc_app_flush_settings(app);
    test_io(app, 0);
    assert(app->saved_settings.sound && !app->settings_dirty);
    app->ui.settings.sound = false;
    mc_app_settings_changed(app);
    mc_app_flush_settings(app);
    host_fail_next(HostStorageWrite);
    mc_app_request_exit(app);
    test_io(app, 0);
    mc_app_process_exit(app, 0);
    assert(app->running && app->settings_dirty && app->failed_io);
    assert(mc_app_wait_timeout(app, 0, 0, 1000) > 0); // No failed-save busy loop
    send(app, InputKeyBack, InputTypeShort);
    assert(!app->ui.exit_pending && app->ui.screen == McScreenTitle);
    mc_app_retry_storage(app);
    test_io(app, 0);
    assert(!app->settings_dirty && !app->failed_io);
    mc_app_free(app);
}

static void test_run_revisions_and_slots(void) {
    host_reset();
    McApp* app = test_app_alloc();
    test_io(app, 0);
    mc_app_start_new_run(app, false);
    test_io(app, 0);
    mc_app_queue_run_save(app, McRunStageActive, true);
    host_defer_io(true);
    mc_app_process_io(app, 0);
    assert(app->worker.job.operation == McIoSaveRun);
    mc_app_select_slot(app, 1);
    assert(app->ui.active_slot == 0 && app->ui.screen == McScreenStorage);
    mc_game_step(&app->ui.game, NULL);
    const uint32_t latest = mc_game_state_hash(&app->ui.game);
    mc_app_queue_run_save(app, McRunStageActive, false);
    host_complete_io();
    mc_app_process_io(app, 0);
    assert(app->run_dirty && app->ui.screen == McScreenStorage);
    assert(atomic_load(&app->worker.state) == McIoPending);
    host_complete_io();
    host_defer_io(false);
    test_io(app, 0);
    assert(!app->run_dirty);
    mc_app_select_slot(app, 1);
    assert(app->ui.active_slot == 1);
    mc_app_select_slot(app, 0);
    app->ui.has_suspended_run = true;
    mc_app_resume_run(app);
    test_io(app, 0);
    assert(mc_game_state_hash(&app->ui.game) == latest);
    // A completion for an obsolete identity cannot replace a different run.
    host_defer_io(true);
    app->pending_io |= McPendingIoRunLoad;
    mc_app_process_io(app, 0);
    app->run_identity++;
    mc_game_step(&app->ui.game, NULL);
    const uint32_t changed = mc_game_state_hash(&app->ui.game);
    host_complete_io();
    host_defer_io(false);
    test_io(app, 0);
    assert(mc_game_state_hash(&app->ui.game) == changed);
    mc_app_free(app);
}

static void test_release_data_paths(void) {
    assert(!strcmp(MC_DATA_FOLDER, MC_APP_VERSION));
    assert(!strcmp(MC_HELP_PATH, APP_DATA_PATH(MC_APP_VERSION "/help.bin")));
    const char* invalid[] = {
        NULL,
        "",
        MC_DATA_FOLDER,
        ".",
        "..",
        "notes",
        "1.2",
        "01.2.3",
        "1.2.3-01",
        "1.2.3-",
        "1.2.3+",
        "1.2.3+build..1",
        "1.2.3/path",
        "1.2.3\\path",
        "v1.2.3"};
    for(unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!mc_cleanup_candidate(invalid[i]));
    const char* valid[] = {"0.0.0", "0.0.1-rc.2+build.001", "0.0.1-01alpha", "v0", "v1"};
    for(unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        if(strcmp(valid[i], MC_DATA_FOLDER)) assert(mc_cleanup_candidate(valid[i]));

    host_reset();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    assert(storage_common_mkdir(storage, APP_DATA_PATH(MC_DATA_FOLDER)) == FSE_OK);
    assert(storage_common_mkdir(storage, APP_DATA_PATH("0.0.1-rc.2+build.001")) == FSE_OK);
    host_load_fixture(MC_HELP_ASSET_PATH, MC_HELP_PATH);
    host_load_fixture(MC_HELP_ASSET_PATH, APP_DATA_PATH("0.0.1-rc.2+build.001/help.bin"));
    McCleanup cleanup;
    mc_cleanup_init(&cleanup, storage);
    for(unsigned step = 0; step < 100 && cleanup.state == McCleanupScanning; step++)
        mc_cleanup_step(&cleanup);
    assert(cleanup.state == McCleanupPrompt && cleanup.count == 1);
    assert(!strcmp(mc_cleanup_name(&cleanup, 0), "0.0.1-rc.2+build.001"));
    assert(storage_file_exists(storage, APP_DATA_PATH("0.0.1-rc.2+build.001/help.bin")));
    mc_cleanup_approve(&cleanup);
    for(unsigned step = 0; step < 100 && cleanup.state == McCleanupPurging; step++)
        mc_cleanup_step(&cleanup);
    assert(cleanup.state == McCleanupDone);
    assert(storage_file_exists(storage, MC_HELP_PATH));
    assert(!storage_file_exists(storage, APP_DATA_PATH("0.0.1-rc.2+build.001/help.bin")));
    mc_cleanup_deinit(&cleanup);
}

static void test_cleanup_navigation(void) {
    host_reset();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    char long_name[81];
    memset(long_name, '2', 80);
    long_name[0] = 'v';
    long_name[80] = 0;
    char path[160];
    snprintf(path, sizeof(path), STORAGE_APP_DATA_PATH_PREFIX "/%s", long_name);
    assert(storage_common_mkdir(storage, path) == FSE_OK);
    McApp* app = mc_app_alloc();
    for(unsigned n = 0; n < 20; n++)
        test_io(app, 0);
    assert(app->ui.cleanup_state == McCleanupPrompt && app->ui.cleanup_length == 80);
    mc_app_cleanup_select(app, true);
    test_io(app, 0);
    assert(app->ui.cleanup_offset == 40);
    mc_app_cleanup_select(app, true);
    test_io(app, 0);
    assert(app->ui.cleanup_offset == 0); // No empty third page for exactly 80 characters.
    mc_app_cleanup_select(app, false);
    test_io(app, 0);
    assert(app->ui.cleanup_offset == 40);
    app->cleanup_action = 1;
    test_io(app, 0);
    assert(!app->cleanup.allocated && app->ui.screen == McScreenTitle);
    mc_app_free(app);
}

static void legacy_checkpoint(McPersistence* p, McGame* game) {
    uint8_t blob[MC_RUN_ENCODED_MAX_SIZE], header[20] = {'M', 'C', 'S', '1'};
    McRunSnapshot run = {.game = game, .stage = McRunStageActive};
    size_t size = mc_run_snapshot_encode(&run, blob, sizeof(blob));
    uint32_t fields[] = {7U, size, 1234U};
    for(unsigned i = 0; i < 3; i++)
        for(unsigned b = 0; b < 4; b++)
            header[4 + 4 * i + b] = fields[i] >> (8 * b);
    uint32_t crc = mc_crc32(header, 16);
    for(unsigned b = 0; b < 4; b++)
        header[16 + b] = crc >> (8 * b);
    File* file = storage_file_alloc(p->storage);
    assert(storage_file_open(
        file, APP_DATA_PATH(MC_DATA_FOLDER "/save0.dat"), FSAM_WRITE, FSOM_CREATE_ALWAYS));
    assert(storage_file_write(file, header, sizeof(header)) == sizeof(header));
    assert(storage_file_write(file, blob, size) == size);
    storage_file_close(file);
    storage_file_free(file);
}
static void test_replay_checkpoint_compatibility(void) {
    host_reset();
    McPersistence p;
    McGame game, restored;
    McWaveStart start, recovered;
    assert(mc_persistence_init(&p, furi_record_open(RECORD_STORAGE)));
    mc_game_start_mode(&game, McDifficultyCrisis, 456, McModeBarrage);
    assert(mc_wave_start_capture(&start, &game));
    McRunSnapshot write = {.game = &game, .stage = McRunStageActive, .wave_start = &start};
    McRunSnapshot read = {.game = &restored, .wave_start = &recovered};
    assert(mc_persistence_checkpoint(&p, &write) == McStorageOk);
    mc_game_step(&game, NULL);
    assert(mc_persistence_checkpoint(&p, &write) == McStorageOk);
    assert(mc_persistence_restore(&p, &read) == McStorageOk);
    assert(mc_game_state_hash(&restored) == mc_game_state_hash(&game));
    assert(recovered.size == start.size && !memcmp(recovered.data, start.data, start.size));
    host_corrupt(APP_DATA_PATH(MC_DATA_FOLDER "/save0.dat"));
    assert(mc_persistence_restore(&p, &read) == McStorageOk && p.recovered);
    assert(mc_wave_start_restore(&recovered, &game));
    assert(mc_game_state_hash(&restored) == mc_game_state_hash(&game));
    assert(mc_persistence_remove_checkpoint(&p) == McStorageOk);
    legacy_checkpoint(&p, &game);
    assert(mc_persistence_restore(&p, &read) == McStorageOk);
    assert(!recovered.size && p.generation == 7U && p.timestamp == 1234U);
    assert(mc_game_state_hash(&restored) == mc_game_state_hash(&game));
    mc_persistence_deinit(&p);
}
static void test_exact_practice_is_unranked_and_repeatable(void) {
    host_reset();
    McApp* app = test_app_alloc();
    test_io(app, 0);
    app->ui.setup_mode = McModeOneCity;
    app->ui.setup_seed = 4321;
    app->ui.settings.show_control_card = false;
    mc_app_start_new_run(app, false);
    test_io(app, 0);
    app->ui.game.phase = McGamePhaseWaveResult;
    app->ui.game.next_supplies = 3;
    app->ui.game.next_radius_boost = 2;
    mc_game_begin_next_wave(&app->ui.game);
    mc_app_capture_wave(app);
    mc_app_queue_run_save(app, McRunStageActive, false);
    test_io(app, 0);
    const McWaveStart wave = app->wave_start;
    // Restart the application and resume the slot before requesting exact practice.
    mc_app_free(app);
    app = test_app_alloc();
    test_io(app, 0);
    mc_app_resume_run(app);
    test_io(app, 0);
    assert(app->ui.has_wave_start && app->ui.active_slot == 0);
    assert(
        app->wave_start.size == wave.size && !memcmp(app->wave_start.data, wave.data, wave.size));
    app->ui.game.score = 999;
    app->ui.game.phase = McGamePhaseGameOver;
    app->ui.screen = McScreenGameOver;
    McProfile profile = app->ui.profile;
    McScoreTables scores = app->ui.scores;
    McSettings settings = app->ui.settings;
    mc_app_practice_wave(app);
    assert(app->ui.wave_practice && app->ui.game.mode == McModeOneCity);
    assert(app->ui.game.supplies == 3 && app->ui.game.radius_boost == 2);
    assert(mc_game_alive_cities(&app->ui.game) == 1);
    const uint32_t hash = mc_game_state_hash(&app->ui.game);
    McGameEventBuffer events = {0};
    mc_game_event_push(&events, McGameEventWaveCleared, 0);
    mc_app_dispatch_events(app, &events);
    assert(app->ui.screen == McScreenPracticeResult);
    assert(!memcmp(&profile, &app->ui.profile, sizeof(profile)));
    assert(!memcmp(&scores, &app->ui.scores, sizeof(scores)));
    assert(!memcmp(&settings, &app->ui.settings, sizeof(settings)));
    mc_app_retry(app, false);
    assert(mc_game_state_hash(&app->ui.game) == hash);
    assert(!(app->pending_io & (McPendingIoRunSave | McPendingIoRunDelete | McPendingIoPaceSave)));
    mc_app_free(app);
}

// Session preparation must preserve fixed-mode policies and exact retry random-call counts.
static void test_retry_all_modes(void) {
    for(unsigned mode = 0; mode < McModeCount; mode++) {
        for(unsigned difficulty = 0; difficulty < McDifficultyCount; difficulty++) {
            host_reset();
            McApp* app = test_app_alloc();
            app->ui.settings.difficulty = difficulty;
            app->ui.setup_mode = mode;
            app->ui.setup_seed = 0x89ABCDEFU;
            mc_app_start_new_run(app, false);
            test_io(app, 0);
            const uint32_t original = mc_game_state_hash(&app->ui.game);
            const unsigned calls = host_random_calls;
            for(unsigned i = 0; i < 9; i++)
                mc_game_step(&app->ui.game, NULL);
            mc_app_retry(app, false);
            assert(host_random_calls == calls);
            assert(mc_game_state_hash(&app->ui.game) == original);
            assert(app->ui.game.difficulty == mc_mode_difficulty(mode, difficulty));
            test_io(app, 0);
            mc_app_retry(app, true);
            assert(host_random_calls == calls + 1);
            assert(app->ui.game.seed == 0x12345678U);
            assert(
                app->ui.game.mode ==
                (mc_mode_def(mode)->seed_policy == McSeedRandom ? mode : McModeClassic));
            test_io(app, 0);
            mc_app_free(app);
        }
    }
}

// Run the retained application, storage, and scripted input-loop checks

static void write_help_fixture(const uint8_t* bytes, size_t size) {
    File* file = storage_file_alloc(furi_record_open(RECORD_STORAGE));
    assert(storage_file_open(file, MC_HELP_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS));
    assert(storage_file_write(file, bytes, size) == size);
    storage_file_close(file);
    storage_file_free(file);
}

// Corruption cases are constructed from independent bytes, not the production encoder.
static void test_help_validation(void) {
    host_reset();
    uint8_t original[8192], bytes[8192];
    FILE* file = fopen(MC_HELP_ASSET_PATH, "rb");
    assert(file);
    const size_t size = fread(original, 1, sizeof(original), file);
    assert(feof(file) && size > 500);
    fclose(file);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    McHelpPage page = {.id = McHelpHud0};
    mc_help_read(storage, &page);
    assert(page.status == McHelpUnavailable);
    write_help_fixture(original, size);
    Canvas canvas = {0};
    canvas_set_font(&canvas, FontSecondary);
    for(unsigned id = 0; id < McHelpPageCount; id++) {
        page.id = id;
        mc_help_read(storage, &page);
        assert(page.status == McHelpReady && page.lines > 0 && page.lines <= 5);
        const char* line = page.text;
        for(unsigned i = 0; i < page.lines; i++) {
            assert(canvas_string_width(&canvas, line) <= (id < McHelpLesson0 ? 122 : 128));
            line += strlen(line) + 1;
        }
        assert(line <= page.text + sizeof(page.text));
    }
    page.id = McHelpHud0;
    for(unsigned fault = 0; fault < 13; fault++) {
        memcpy(bytes, original, size);
        size_t length = size;
        // First index entry has a little-endian body offset; first page has multiple lines.
        const unsigned offset = bytes[12] | bytes[13] << 8;
        switch(fault) {
        case 0:
            bytes[0] ^= 1;
            break; // magic/version
        case 1:
            bytes[4] ^= 1;
            break; // catalogue identity
        case 2:
            bytes[8]--;
            break; // page count
        case 3:
            bytes[10]++;
            break; // index size
        case 4:
            length = 11;
            break; // truncated header
        case 5:
            bytes[12] = 0;
            bytes[13] = 0;
            break; // body aliases directory
        case 6:
            bytes[12] = 0xFF;
            bytes[13] = 0xFF;
            break; // offset outside file
        case 7:
            bytes[14] = 129;
            break; // oversized page
        case 8:
            bytes[14] = 0;
            break; // empty page
        case 9:
            bytes[15] = 6;
            break; // too many lines
        case 10:
            bytes[16] ^= 1;
            break; // checksum
        case 11:
            bytes[offset + bytes[14] - 1] = 'X';
            break; // missing termination
        case 12:
            length = offset + bytes[14] - 1;
            break; // truncated page
        }
        write_help_fixture(bytes, length);
        mc_help_read(storage, &page);
        assert(page.status == McHelpUnavailable);
    }
    write_help_fixture(original, size);
    const HostStorageOperation faults[] = {HostStorageOpen, HostStorageRead, HostStorageSeek};
    for(unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        host_fail_next(faults[i]);
        mc_help_read(storage, &page);
        assert(page.status == McHelpUnavailable);
        mc_help_read(storage, &page);
        assert(page.status == McHelpReady);
    }
    // Valid CRC alone must not admit empty lines, control characters, or bad line counts.
    uint8_t entry[8] = {0, 0, 4, 2};
    memcpy(page.text, "a\0b", 4);
    mc_write_u32(entry + 4, mc_crc32((uint8_t*)page.text, 4));
    assert(mc_help_page_valid(entry, &page));
    entry[3] = 1;
    assert(!mc_help_page_valid(entry, &page));
    entry[3] = 2;
    page.text[0] = 0;
    mc_write_u32(entry + 4, mc_crc32((uint8_t*)page.text, 4));
    assert(!mc_help_page_valid(entry, &page));
    page.text[0] = '\n';
    mc_write_u32(entry + 4, mc_crc32((uint8_t*)page.text, 4));
    assert(!mc_help_page_valid(entry, &page));
}

static void test_help_coordination(void) {
    host_reset();
    McApp* app = test_app_alloc();
    host_load_fixture(MC_HELP_ASSET_PATH, MC_HELP_PATH);
    app->ui.screen = McScreenHudGuide;
    app->ui.guide_page = 0;
    app->ui.settings.sound = false;
    mc_app_settings_changed(app);
    mc_app_flush_settings(app);
    host_defer_io(true);
    mc_app_process_io(app, 0);
    assert(app->worker.job.operation == McIoSaveSettings); // Required writes precede help.
    assert(app->ui.help.status == McHelpLoading);
    host_complete_io();
    mc_app_process_io(app, 0);
    assert(app->worker.job.operation == McIoHelp);
    assert(app->worker.job.data.help.id == McHelpHud0);
    // Navigate while this request owns its buffer. The obsolete page must never be published.
    send(app, InputKeyRight, InputTypePress);
    assert(app->ui.guide_page == 1);
    Canvas canvas = {0};
    mc_view_draw_callback(&canvas, app);
    host_complete_io();
    mc_app_process_io(app, 0);
    assert(app->ui.help.id == McHelpHud1 && app->ui.help.status == McHelpLoading);
    assert(app->worker.job.operation == McIoHelp && app->worker.job.data.help.id == McHelpHud1);
    host_complete_io();
    host_defer_io(false);
    test_io(app, 0);
    assert(app->ui.help.status == McHelpReady);
    McRenderSnapshot snapshot;
    mc_render_snapshot(&snapshot, &app->ui);
    const char saved = snapshot.help.text[0];
    app->ui.help.text[0] ^= 1;
    assert(snapshot.help.text[0] == saved); // Snapshot owns its text.
    // A rapid round trip to the same ID must reject even a failed old reply.
    send(app, InputKeyRight, InputTypePress);
    host_defer_io(true);
    mc_app_process_io(app, 0);
    const uint32_t old_revision = app->worker.job.data.help.revision;
    send(app, InputKeyRight, InputTypePress);
    send(app, InputKeyRight, InputTypePress);
    assert(app->ui.help.id == app->worker.job.data.help.id);
    assert(app->ui.help.revision != old_revision);
    host_fail_next(HostStorageRead);
    host_complete_io();
    mc_app_process_io(app, 0);
    assert(app->ui.help.status == McHelpLoading);
    assert(app->worker.job.operation == McIoHelp);
    assert(app->worker.job.data.help.revision != old_revision);
    host_complete_io();
    host_defer_io(false);
    test_io(app, 0);
    assert(app->ui.help.status == McHelpReady);
    // Missing help is isolated from save errors and retries only after reopening.
    app->ui.screen = McScreenTitle;
    test_io(app, 0);
    assert(storage_common_remove(app->persistence.storage, MC_HELP_PATH) == FSE_OK);
    app->ui.screen = McScreenHudGuide;
    const McPendingIo failed = app->failed_io;
    const McStorageResult result = app->ui.storage_result;
    test_io(app, 0);
    assert(app->ui.help.status == McHelpUnavailable);
    host_load_fixture(MC_HELP_ASSET_PATH, MC_HELP_PATH);
    test_io(app, 100000);
    assert(app->ui.help.status == McHelpUnavailable && app->failed_io == failed);
    assert(app->ui.storage_result == result);
    app->ui.screen = McScreenTitle;
    test_io(app, 100000);
    app->ui.screen = McScreenHudGuide;
    test_io(app, 100000);
    assert(app->ui.help.status == McHelpReady);
    app->ui.screen = McScreenPlaying;
    test_io(app, 100000);
    assert(app->ui.help.id == McHelpNoPage);
    app->ui.screen = McScreenHudGuide;
    host_defer_io(true);
    mc_app_process_io(app, 100000);
    assert(app->worker.job.operation == McIoHelp);
    mc_app_request_exit(app);
    host_complete_io();
    host_defer_io(false);
    test_io(app, 100000);
    assert(app->ui.help.id == McHelpNoPage && app->ui.help.status == McHelpEmpty);
    mc_app_free(app);
}

// Simple pages accept completed OK/Back gestures, but only physical direction repeats page.
static void test_simple_page_events(void) {
    const McScreen screens[] = {
        McScreenPuzzleHint,
        McScreenScoreDetails,
        McScreenRunStats,
        McScreenAbout,
        McScreenHudGuide};
    const InputType types[] = {
        InputTypePress, InputTypeRelease, InputTypeShort, InputTypeLong, InputTypeRepeat};
    const InputKey keys[] = {
        InputKeyUp, InputKeyDown, InputKeyLeft, InputKeyRight, InputKeyOk, InputKeyBack};
    host_reset();
    McApp* app = test_app_alloc();
    for(unsigned s = 0; s < sizeof(screens) / sizeof(screens[0]); s++) {
        for(unsigned t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
            for(unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
                for(unsigned page = 0; page < 2; page++) {
                    app->ui.screen = screens[s];
                    app->ui.detail_return_screen = McScreenPaused;
                    app->ui.menu_index = 3;
                    app->ui.stats_page = app->ui.guide_page = page;
                    const bool ok = types[t] == InputTypeShort && keys[k] == InputKeyOk;
                    const bool back = types[t] == InputTypeShort && keys[k] == InputKeyBack;
                    const bool horizontal =
                        (types[t] == InputTypePress || types[t] == InputTypeRepeat) &&
                        (keys[k] == InputKeyLeft || keys[k] == InputKeyRight);
                    const bool guide = screens[s] == McScreenHudGuide;
                    const bool leave = back || (ok && !guide);
                    send(app, keys[k], types[t]);
                    assert(
                        app->ui.screen ==
                        (leave ? (screens[s] == McScreenAbout ? McScreenTitle : McScreenPaused) :
                                 screens[s]));
                    assert(app->ui.menu_index == (leave && screens[s] == McScreenAbout ? 0 : 3));
                    assert(
                        app->ui.stats_page ==
                        (page ^ (screens[s] == McScreenRunStats && horizontal)));
                    assert(app->ui.guide_page == (page ^ (guide && (horizontal || ok))));
                    if(guide && !leave) assert(app->ui.help.id == McHelpHud0 + app->ui.guide_page);
                    if(leave) assert(app->ui.help.id == McHelpNoPage);
                }
            }
        }
    }
    mc_app_free(app);
}

// Fixture mutations deliberately use independent byte operations and CRC calculation.
static void checkpoint_u32(uint8_t* p, uint32_t value) {
    for(unsigned b = 0; b < 4; b++)
        p[b] = value >> (8 * b);
}
static uint32_t checkpoint_crc(const uint8_t* bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for(size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for(unsigned b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0U);
    }
    return ~crc;
}
static void
    checkpoint_fixture(Storage* storage, const char* path, const uint8_t* bytes, size_t size) {
    File* file = storage_file_alloc(storage);
    assert(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS));
    assert(storage_file_write(file, bytes, size) == size);
    storage_file_close(file);
    storage_file_free(file);
}
static void test_checkpoint_boundaries(void) {
    host_reset();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    McPersistence p;
    assert(mc_persistence_init(&p, storage));
    McGame game, restored;
    McWaveStart start, replay;
    mc_game_start_mode(&game, McDifficultyCrisis, 456, McModeBarrage);
    assert(mc_wave_start_capture(&start, &game));
    McRunSnapshot source = {.game = &game, .stage = McRunStageActive, .wave_start = &start};
    assert(mc_persistence_checkpoint(&p, &source) == McStorageOk);
    const char* primary = APP_DATA_PATH(MC_DATA_FOLDER "/save0.dat");
    const char* backup = APP_DATA_PATH(MC_DATA_FOLDER "/save0.bak");
    uint8_t original[2048], valid[2048], bytes[2048];
    File* file = storage_file_alloc(storage);
    assert(storage_file_open(file, primary, FSAM_READ, FSOM_OPEN_EXISTING));
    const size_t original_size = storage_file_size(file);
    assert(original_size <= sizeof(original));
    assert(storage_file_read(file, original, original_size) == original_size);
    storage_file_close(file);
    storage_file_free(file);
    const size_t run_size = original[8] | (size_t)original[9] << 8;
    for(unsigned v2 = 0; v2 < 2; v2++) {
        for(unsigned has_replay = 0; has_replay <= v2; has_replay++) {
            const size_t header = v2 ? 24 : 20;
            const size_t size = header + run_size + (has_replay ? start.size : 0);
            memcpy(valid, original, 16);
            valid[3] = v2 ? '2' : '1';
            if(v2) checkpoint_u32(valid + 16, has_replay ? start.size : 0);
            checkpoint_u32(valid + header - 4, checkpoint_crc(valid, header - 4));
            memcpy(valid + header, original + 24, size - header);
            for(unsigned want_replay = 0; want_replay < 2; want_replay++) {
                McRunSnapshot target = {
                    .game = &restored, .wave_start = want_replay ? &replay : NULL};
                for(unsigned fault = 0; fault < 13; fault++) {
                    memcpy(bytes, valid, size);
                    size_t length = size;
                    switch(fault) {
                    case 1:
                        length = 19;
                        break;
                    case 2:
                        length = header - 1;
                        break;
                    case 3:
                        length = header;
                        break;
                    case 4:
                        length = header + run_size - 1;
                        break;
                    case 5:
                        length = size - 1;
                        break;
                    case 6:
                        bytes[0] ^= 1;
                        break;
                    case 7:
                        bytes[header - 1] ^= 1;
                        break;
                    case 8:
                        bytes[header + run_size - 1] ^= 1;
                        break;
                    case 9:
                        checkpoint_u32(bytes + 8, 0);
                        break;
                    case 10:
                        checkpoint_u32(bytes + 8, UINT32_MAX);
                        break;
                    case 11:
                        if(v2)
                            checkpoint_u32(bytes + 16, MC_WAVE_START_MAX_SIZE + 1);
                        else
                            bytes[0] ^= 1;
                        break;
                    case 12:
                        if(has_replay)
                            bytes[size - 1] ^= 1;
                        else {
                            bytes[size] = 0;
                            length++;
                        }
                        break;
                    }
                    if(fault >= 9 && fault <= 11)
                        checkpoint_u32(bytes + header - 4, checkpoint_crc(bytes, header - 4));
                    checkpoint_fixture(storage, primary, bytes, length);
                    p.generation = 123;
                    p.timestamp = 456;
                    const McStorageResult result = mc_persistence_restore(&p, &target);
                    assert(
                        result == (!fault           ? McStorageOk :
                                   v2 && fault == 2 ? McStorageIoError :
                                                      McStorageInvalid));
                    if(!fault) {
                        assert(mc_game_state_hash(&restored) == mc_game_state_hash(&game));
                        if(want_replay) assert(replay.size == (has_replay ? start.size : 0));
                    } else
                        assert(p.generation == 123 && p.timestamp == 456);
                }
                // A good backup wins; an invalid primary outranks a missing backup.
                checkpoint_fixture(storage, backup, valid, size);
                assert(mc_persistence_restore(&p, &target) == McStorageOk && p.recovered);
                assert(storage_common_remove(storage, backup) == FSE_OK);
                checkpoint_fixture(storage, primary, valid, size);
                host_fail_next(HostStorageOpen);
                assert(mc_persistence_restore(&p, &target) == McStorageIoError);
                host_fail_next(HostStorageRead);
                assert(mc_persistence_restore(&p, &target) == McStorageInvalid);
                // MCS2 adds its header read before the snapshot and optional replay read.
                for(unsigned read = 1; read < 2 + v2 + has_replay; read++) {
                    host_fail_operation_after(HostStorageRead, read);
                    assert(mc_persistence_restore(&p, &target) == McStorageIoError);
                }
                checkpoint_fixture(storage, backup, valid, size);
                checkpoint_fixture(storage, primary, valid, 19);
                host_fail_next(HostStorageRead);
                assert(mc_persistence_restore(&p, &target) == McStorageInvalid);
                assert(storage_common_remove(storage, primary) == FSE_OK);
                host_fail_next(HostStorageRead);
                assert(mc_persistence_restore(&p, &target) == McStorageInvalid);
                host_fail_next(HostStorageOpen);
                assert(mc_persistence_restore(&p, &target) == McStorageIoError);
                assert(storage_common_remove(storage, backup) == FSE_OK);
            }
        }
    }
    // The optional replay must still match when its output pointer is absent.
    mc_game_start_mode(&game, McDifficultyCrisis, 457, McModeBarrage);
    assert(mc_wave_start_capture(&start, &game));
    memcpy(original + 24 + run_size, start.data, start.size);
    checkpoint_fixture(storage, primary, original, original_size);
    McRunSnapshot target = {.game = &restored};
    assert(mc_persistence_restore(&p, &target) == McStorageInvalid);
    // Invalid primary wins over a failed backup; failed primary wins over invalid backup.
    checkpoint_fixture(storage, backup, original, original_size);
    host_fail_next(HostStorageOpen);
    assert(mc_persistence_restore(&p, &target) == McStorageIoError);
    mc_persistence_deinit(&p);
}

int main(void) {
    test_countdown_play_transitions();
    test_resume_timer_setting_survives_restart();
    test_simple_page_events();
    test_checkpoint_boundaries();
    test_retry_all_modes();
    test_help_validation();
    test_help_coordination();
    test_input_fifo_and_overflow();
    test_callback_battery_gestures();
    test_render_snapshot_consistency();
    test_inverted_frames_and_overlays();
    test_invert_setting_survives_restart();
    test_worker_edits_and_exit();
    test_run_revisions_and_slots();
    test_cleanup_navigation();
    test_release_data_paths();
    test_replay_checkpoint_compatibility();
    test_exact_practice_is_unranked_and_repeatable();
    test_storage_transactions();
    test_retention();
    test_ui_and_save_resume();
    test_settings_and_setup();
    test_real_application_loop();
    puts("core application, save/recovery, and input-loop tests passed");
    return 0;
}
