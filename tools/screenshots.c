// Curated documentation screens and gameplay scenes rendered by the real application code
#include "app_internal.h"
#include "render.h"
#include "help_ui.h"
#include "help_storage.h"
#include "host_platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCREENSHOT_SEED 0x12345678U
static bool read_help = true;

// Reset each scenario so earlier captures cannot leak settings, overlays, or cached text
static void prepare(McApp* app) {
    memset(app, 0, sizeof(*app));
    mc_settings_defaults(&app->ui.settings);
    mc_score_tables_defaults(&app->ui.scores);
    mc_profile_defaults(&app->ui.profile);
    mc_game_init(&app->ui.game);
    app->ui.setup_seed = SCREENSHOT_SEED;
    app->ui.score_difficulty = McDifficultyCommand;
}

// Save a monochrome frame and ensure drawing never mutates the prepared UI state
static void capture(const char* directory, const char* name, McApp* app, McScreen screen) {
    app->ui.screen = screen;
    mc_app_refresh_render_cache(app);
    if(screen != McScreenCleanup) {
        app->ui.help.id = mc_help_requested(&app->ui.common, &app->ui.game);
        if(read_help && app->ui.help.id != McHelpNoPage)
            mc_help_read(furi_record_open(RECORD_STORAGE), &app->ui.help);
    }
    const McUiModel before = app->ui;
    Canvas canvas = {0};
    McRenderSnapshot snapshot;
    mc_render_snapshot(&snapshot, &app->ui);
    mc_render(&canvas, &snapshot);
    assert(memcmp(&before, &app->ui, sizeof(before)) == 0);
    char path[1024];
    const int length = snprintf(path, sizeof(path), "%s/%s.pgm", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE* file = fopen(path, "wb");
    assert(file);
    assert(fprintf(file, "P5\n128 64\n1\n") > 0);
    assert(fwrite(canvas.pixels, 1U, sizeof(canvas.pixels), file) == sizeof(canvas.pixels));
    assert(fclose(file) == 0);
    puts(name);
}

// Move the real game cursor to an exact pixel target before firing or showing aiming aids
static void aim(McGame* game, uint8_t x, uint8_t y) {
    mc_game_move_cursor_q8(
        game,
        (int16_t)((int32_t)x * MC_CURSOR_ONE - game->cursor_x_q8),
        (int16_t)((int32_t)y * MC_CURSOR_ONE - game->cursor_y_q8));
}

// Capture the main navigation and configuration pages without duplicating every option state
static void capture_pages(const char* output) {
    McApp app;
    prepare(&app);
    capture(output, "title", &app, McScreenTitle);
    capture(output, "setup", &app, McScreenRunSetup);
    capture(output, "controls", &app, McScreenControlCard);

    prepare(&app);
    app.ui.wave_practice = true;
    app.ui.game.victory = true;
    capture(output, "wave_practice_result", &app, McScreenPracticeResult);
    prepare(&app);
    app.ui.exit_pending = app.ui.exit_slow = true;
    app.ui.storage_result = McStorageIoError;
    capture(output, "save_before_exit", &app, McScreenExiting);
    app.ui.confirm_action = McConfirmExitUnsaved;
    capture(output, "discard_unsaved", &app, McScreenConfirm);

    prepare(&app);
    app.ui.settings_group = 0U;
    app.ui.menu_index = McSettingsItemCursor;
    capture(output, "settings", &app, McScreenSettings);

    prepare(&app);
    app.ui.cleanup_state = McCleanupPrompt;
    app.ui.cleanup_can_migrate = true;
    app.ui.cleanup_count = 3;
    app.ui.menu_index = 1;
    strcpy(app.ui.cleanup_source, "0.9.0+build.2");
    capture(output, "migration", &app, McScreenCleanup);
    app.ui.cleanup_state = McCleanupMigrating;
    app.ui.cleanup_migrating = true;
    capture(output, "migration_progress", &app, McScreenCleanup);
    app.ui.cleanup_state = McCleanupValidating;
    capture(output, "migration_upgrade", &app, McScreenCleanup);
    app.ui.cleanup_state = McCleanupFailed;
    app.ui.storage_result = McStorageInvalid;
    capture(output, "migration_failed", &app, McScreenCleanup);

    prepare(&app);
    app.ui.settings_group = 3;
    app.ui.menu_index = McSettingsItemVersionData;
    app.ui.settings_return_screen = McScreenTitle;
    capture(output, "version_data_settings", &app, McScreenSettings);
    app.ui.cleanup_state = McCleanupPrompt;
    app.ui.cleanup_count = 3;
    strcpy(app.ui.cleanup_name, "0.9.0+build.2");
    app.ui.menu_index = 1;
    capture(output, "cleanup_preview", &app, McScreenCleanup);
    app.ui.cleanup_confirm = true;
    app.ui.menu_index = 0;
    capture(output, "cleanup_confirm", &app, McScreenCleanup);

    prepare(&app);
    app.ui.settings_group = 1U;
    app.ui.menu_index = McSettingsItemInvertColors;
    app.ui.settings.invert_colors = true;
    capture(output, "inverted_display", &app, McScreenSettings);

    prepare(&app);
    app.ui.setup_mode = McModePractice;
    app.ui.setup_options = (McRunOptions){.start_wave = 9U};
    capture(output, "practice_options", &app, McScreenPracticeSetup);

    prepare(&app);
    app.ui.setup_mode = McModePuzzle;
    app.ui.setup_options.start_wave = 2U;
    capture(output, "puzzle_options", &app, McScreenPracticeSetup);

    prepare(&app);
    app.ui.slots[0] = (McSaveInfo){
        .score = 12450U,
        .wave = 10U,
        .mode = McModeClassic,
        .difficulty = McDifficultyCommand,
        .status = McStorageOk,
        .timestamp = 1788480000U};
    app.ui.slots[1].status = app.ui.slots[2].status = McStorageMissing;
    capture(output, "saved_runs", &app, McScreenSlots);

    prepare(&app);
    for(uint8_t i = 0U; i < MC_SCORE_CAPACITY; i++) {
        const McScoreEntry entry = {
            .score = 25000U - i * 3700U,
            .wave = 16U - i * 2U,
            .difficulty = McDifficultyCommand,
            .cities_remaining = 3U,
        };
        assert(mc_score_tables_insert(&app.ui.scores, &entry));
    }
    capture(output, "high_scores", &app, McScreenHighScores);

    prepare(&app);
    app.ui.profile.earned_medals = (1U << McMedalChainReaction) | (1U << McMedalPerfectDefense);
    capture(output, "medals", &app, McScreenMedals);

    prepare(&app);
    mc_game_start_mode(&app.ui.game, McDifficultyCadet, SCREENSHOT_SEED, McModeTraining);
    capture(output, "training", &app, McScreenLesson);
}

// Advance an authored firing sequence through real simulation for a representative active wave
static void prepare_gameplay(McApp* app, McGameMode mode, uint16_t wave) {
    prepare(app);
    McGame* game = &app->ui.game;
    mc_game_start_mode(game, McDifficultyCommand, SCREENSHOT_SEED, mode);
    game->wave = wave - 1U;
    game->phase = McGamePhaseWaveResult;
    mc_game_begin_next_wave(game);
    game->score = 12450U;
    app->ui.high_score = 25000U;
    for(uint16_t tick = 0U; tick < 85U; tick++) {
        if(tick == 58U || tick == 76U) {
            aim(game, tick == 58U ? 43U : 82U, tick == 58U ? 28U : 35U);
            assert(mc_game_fire(game) == McFireSuccess);
        }
        mc_game_step(game, NULL);
    }
    assert(mc_game_validate(game) && game->enemy_count > 0U);
}

// Show distinct gameplay presentations rather than a matrix of transient warning overlays
static void capture_gameplay(const char* output) {
    McApp app;
    prepare_gameplay(&app, McModeClassic, 10U);
    capture(output, "gameplay", &app, McScreenPlaying);
    capture(output, "paused", &app, McScreenPaused);
    app.ui.input_recovery = McInputRecoveryHeld;
    capture(output, "input_interrupted", &app, McScreenPaused);
    app.ui.input_recovery = McInputRecoveryReady;
    capture(output, "input_resume", &app, McScreenPaused);
    app.ui.input_recovery = McInputRecoveryNone;
    app.ui.settings.simple_hud = true;
    capture(output, "simple_hud", &app, McScreenPlaying);
    app.ui.settings.simple_hud = false;
    app.ui.battery_overlay = true;
    capture(output, "battery_select", &app, McScreenPlaying);

    prepare_gameplay(&app, McModeClassic, 25U);
    capture(output, "gameplay_late", &app, McScreenPlaying);

    prepare_gameplay(&app, McModeSprint, 10U);
    capture(output, "score_attack", &app, McScreenPlaying);

    prepare(&app);
    const McRunOptions options = {.start_wave = 24U};
    mc_game_start_config(&app.ui.game, McDifficultyCommand, 42U, McModePractice, &options);
    aim(&app.ui.game, 72U, 30U);
    for(unsigned tick = 0U; tick < 40U; tick++)
        mc_game_step(&app.ui.game, NULL);
    assert(mc_game_validate(&app.ui.game));
    capture(output, "practice_aiming", &app, McScreenPlaying);
}

// Let the simulation produce real wave results, repairable damage, and an eventual defeat
static void capture_results(const char* output) {
    McApp app;
    prepare(&app);
    McGame* game = &app.ui.game;
    mc_game_start_run(game, McDifficultyCommand, SCREENSHOT_SEED);
    game->score = 12450U;
    mc_app_capture_wave(&app);
    for(unsigned tick = 0U; tick < 3000U && game->phase == McGamePhasePlaying; tick++)
        mc_game_step(game, NULL);
    assert(mc_game_validate(game) && game->phase == McGamePhaseWaveResult);
    capture(output, "wave_result", &app, McScreenWaveResult);
    for(uint8_t site = 0U; site < MC_SITE_COUNT; site++) {
        if(mc_game_site_def(site)->kind == McSiteCity && !mc_game_site_alive(game, site)) {
            app.ui.repair_site_index = site;
            break;
        }
    }
    assert(mc_game_repair_reason(game, app.ui.repair_site_index) == McWorkshopOk);
    capture(output, "workshop", &app, McScreenRepair);

    for(unsigned tick = 0U; tick < 10000U && game->phase != McGamePhaseGameOver; tick++) {
        if(game->phase == McGamePhaseWaveResult) {
            mc_game_begin_next_wave(game);
            mc_app_capture_wave(&app);
        }
        mc_game_step(game, NULL);
    }
    assert(mc_game_validate(game) && game->phase == McGamePhaseGameOver);
    capture(output, "game_over", &app, McScreenGameOver);
}

// Emit only the curated scene names consumed by the PNG writer
int main(int argc, char** argv) {
    if(argc != 2) {
        fprintf(stderr, "Usage: %s OUTPUT_DIRECTORY\n", argv[0]);
        return 2;
    }
    host_reset();
    host_load_fixture(MC_HELP_ASSET_PATH, MC_HELP_PATH);
    capture_pages(argv[1]);
    capture_gameplay(argv[1]);
    capture_results(argv[1]);
    McApp app;
    prepare(&app);
    read_help = false;
    app.ui.help.status = McHelpLoading;
    capture(argv[1], "help_loading", &app, McScreenHudGuide);
    app.ui.help.status = McHelpUnavailable;
    capture(argv[1], "help_unavailable", &app, McScreenHudGuide);
    app.ui.game.wave = 4;
    capture(argv[1], "training_without_help", &app, McScreenLesson);
    return 0;
}
