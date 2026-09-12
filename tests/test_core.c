// Core deterministic gameplay, fixed-memory limits, and save-format regression checks
#include "balance.h"
#include "binary.h"
#include "collision.h"
#include "controls.h"
#include "game.h"
#include "persistence_codec.h"
#include "runtime.h"
#include "wave.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Rewrite a little-endian field when constructing or resealing a malformed save blob
static void write_u32(uint8_t* output, uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

// Find a notification type within the populated portion of a simulation event buffer
static bool has_event(const McGameEventBuffer* events, McGameEventType type) {
    for(uint8_t i = 0U; i < events->count; i++) {
        if(events->items[i].type == type) return true;
    }
    return false;
}

// Check that every mode and difficulty starts valid and remains valid after initial ticks
static void test_mode_startup(void) {
    for(uint8_t mode = 0; mode < McModeCount; mode++) {
        for(uint8_t difficulty = 0; difficulty < McDifficultyCount; difficulty++) {
            McGame game;
            mc_game_start_mode(&game, difficulty, 42U, mode);
            assert(game.mode == mode && mc_game_validate(&game));
            for(unsigned i = 0; i < 30; i++)
                mc_game_step(&game, NULL);
            assert(mc_game_validate(&game));
        }
    }
}

// Keep host-visible simulation structures within the application's fixed memory budgets
static void test_structure_budgets(void) {
    assert(sizeof(McPath) == 12U);
    assert(sizeof(McGameEvent) == 2U);
    assert(sizeof(McGameEventBuffer) <= 96U);
    assert(sizeof(McGame) <= 1024U);
}

// Check fractional simulation carry, stall limits, pause resets, and adaptive render choices
static void test_rational_scheduler_and_render_modes(void) {
    uint32_t accumulator = 0U;
    uint32_t steps = 0U;
    // Uneven wait lengths must still add up to exactly one second of simulation
    for(uint16_t elapsed = 0U; elapsed < 1000U; elapsed += 7U) {
        const uint16_t chunk = elapsed + 7U > 1000U ? (uint16_t)(1000U - elapsed) : 7U;
        steps += mc_runtime_steps(&accumulator, chunk, 1000U, true, true);
    }
    assert(steps == 30U && accumulator == 0U);
    // A long stall drops excess whole steps while retaining the fractional next-step remainder
    assert(mc_runtime_steps(&accumulator, 10001U, 1000U, true, true) == MC_MAX_CATCH_UP_STEPS);
    assert(accumulator == 30U);
    assert(mc_runtime_steps(&accumulator, 0U, 1000U, true, true) == 0U);
    assert(accumulator == 30U);
    assert(mc_runtime_steps(&accumulator, 33U, 1000U, true, true) == 1U);
    assert(accumulator == 20U);
    assert(mc_runtime_steps(&accumulator, 1000U, 1000U, true, false) == 0U);
    assert(accumulator == 0U);
    assert(mc_runtime_steps(&accumulator, 1000U, 1000U, false, true) == 0U);
    assert(accumulator == 0U);

    McGame game;
    mc_game_init(&game);
    assert(mc_render_target_fps(&game, McRenderAdaptive) == 0U);
    game.enemy_count = 1U;
    assert(mc_render_target_fps(&game, McRenderAdaptive) == 20U);
    game.enemy_count = 4U;
    assert(mc_render_target_fps(&game, McRenderAdaptive) == 30U);
    assert(mc_render_target_fps(&game, McRenderBatterySaver) == 15U);
}

// Verify that a fractional trajectory stays in bounds and lands exactly on its final tick
static void test_q9_path_exact_arrival(void) {
    McPath path;
    mc_path_init(&path, 2, 9, 125, 50, 37U);
    for(uint16_t i = 0U; i < 36U; i++) {
        assert(!mc_path_step(&path));
        assert(mc_path_x(&path) < MC_SCREEN_WIDTH);
        assert(mc_path_y(&path) < MC_SCREEN_HEIGHT);
    }
    assert(mc_path_step(&path));
    assert(path.remaining == 0U);
    assert(mc_path_x(&path) == 125U);
    assert(mc_path_y(&path) == 50U);
}

// Check deterministic roster depletion and bounded pressure changes from wave modifiers
static void test_weighted_roster_and_modifiers(void) {
    McGame left;
    McGame right;
    mc_game_start_run(&left, McDifficultyCommand, 0x12345678U);
    mc_game_start_run(&right, McDifficultyCommand, 0x12345678U);
    assert(left.pending_remaining == 8U);
    assert(memcmp(left.roster_remaining, right.roster_remaining, 3U) == 0);
    while(left.pending_remaining > 0U) {
        assert(mc_wave_take_next_kind(&left) == mc_wave_take_next_kind(&right));
    }
    assert(left.roster_remaining[0] == 0U);
    assert(left.roster_remaining[1] == 0U);
    assert(left.roster_remaining[2] == 0U);

    McWaveBalance base = mc_balance_for_wave(20U, McDifficultyCrisis, McWaveModifierNone);
    McWaveBalance barrage = mc_balance_for_wave(20U, McDifficultyCrisis, McWaveModifierBarrage);
    McWaveBalance velocity = mc_balance_for_wave(20U, McDifficultyCrisis, McWaveModifierVelocity);
    McWaveBalance overload = mc_balance_for_wave(20U, McDifficultyCrisis, McWaveModifierOverload);
    assert(barrage.spawn_interval_ticks >= 6U);
    assert(barrage.spawn_interval_ticks < base.spawn_interval_ticks);
    assert(velocity.kind_speed_x100[McEnemyStandard] <= 2200U);
    assert(velocity.kind_speed_x100[McEnemyStandard] > base.kind_speed_x100[McEnemyStandard]);
    assert(overload.max_active_enemies <= 8U);
    assert(base.enemy_total == MC_PENDING_CAPACITY);
    assert(
        mc_balance_for_wave(1U, McDifficultyCommand, McWaveModifierNone)
            .kind_speed_x100[McEnemyStandard] == 413U);

    left.wave = 20U;
    left.modifier = McWaveModifierFastSwarm;
    mc_wave_prepare(&left, MC_PENDING_CAPACITY);
    assert(left.roster_remaining[1] >= left.roster_remaining[0]);
    left.modifier = McWaveModifierFracture;
    mc_wave_prepare(&left, MC_PENDING_CAPACITY);
    assert(left.roster_remaining[2] > 0U);
    left.modifier = McWaveModifierFocusFire;
    left.alive_site_mask &= (uint16_t)~1U;
    left.priority_site = 0U;
    mc_wave_choose_target(&left);
    assert(left.priority_site != 0U);
    assert(mc_game_site_alive(&left, left.priority_site));
}

// Ensure modifier selection avoids repeats without consuming the targeting random stream
static void test_director_rng_isolated(void) {
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 55U);
    const uint32_t targeting_rng = game.rng_state;
    game.modifier = McWaveModifierBarrage;
    const McWaveModifier first = mc_wave_select_next_modifier(&game, 10U);
    assert(first != McWaveModifierNone && first != game.modifier);
    assert(game.rng_state == targeting_rng);
    game.modifier = first;
    assert(mc_wave_select_next_modifier(&game, 11U) != first);
}

// Compare state hashes, redraw flags, and event counts for two identically seeded runs
static void test_deterministic_simulation(void) {
    McGame left;
    McGame right;
    mc_game_start_run(&left, McDifficultyCadet, 0x12345678U);
    mc_game_start_run(&right, McDifficultyCadet, 0x12345678U);
    for(uint16_t tick = 0U; tick < 250U; tick++) {
        McGameEventBuffer left_events;
        McGameEventBuffer right_events;
        assert(mc_game_step(&left, &left_events) == mc_game_step(&right, &right_events));
        assert(left_events.count == right_events.count);
        assert(!left_events.overflowed && !right_events.overflowed);
        assert(mc_game_state_hash(&left) == mc_game_state_hash(&right));
    }
}

// Verify manual battery choice, ammo accounting, occupied shot slots, and refusal when full
static void test_masks_and_battery_lock(void) {
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 77U);
    assert(mc_game_select_battery(&game, McBatteryLeft));
    assert(mc_game_selected_battery(&game) == 0);
    game.battery_ammo[0] = 0U;
    assert(mc_game_fire(&game) == McFireEmpty);
    assert(mc_game_select_battery(&game, McBatteryCenter));
    assert(mc_game_fire(&game) == McFireSuccess);
    assert(game.interceptor_mask == 1U && game.interceptor_count == 1U);
    assert(game.battery_ammo[1] == MC_AMMO_PER_BATTERY - 1U);
    for(uint8_t i = 1U; i < MC_INTERCEPTOR_CAPACITY; i++)
        assert(mc_game_fire(&game) == McFireSuccess);
    assert(game.interceptor_count == MC_INTERCEPTOR_CAPACITY);
    assert(mc_game_fire(&game) == McFirePoolFull);
    assert(mc_game_select_battery(&game, McBatteryRight));
    assert(game.battery_selection == McBatteryRight);
    assert(mc_game_select_battery(&game, McBatteryAuto));
    assert(game.battery_selection == McBatteryAuto);
    assert(!mc_game_select_battery(&game, McBatterySelectionCount));
    assert(game.battery_selection == McBatteryAuto);
}

// Check fallback after battery loss and reject later attempts to select destroyed sites
static void test_destroyed_selected_battery_falls_back(void) {
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 91U);
    game.spawn_cooldown = 100U;
    game.roster_remaining[0] = 1U;
    game.roster_remaining[1] = 0U;
    game.roster_remaining[2] = 0U;
    game.pending_remaining = 1U;
    game.battery_selection = McBatteryLeft;

    const McSiteDef* left = mc_game_site_def(0U);
    game.enemy_mask = 1U;
    game.enemy_count = 1U;
    game.enemies[0].kind = McEnemyStandard;
    game.enemies[0].target_site = 0U;
    mc_path_init(&game.enemies[0].path, left->x, left->y, left->x, left->y, 1U);

    McGameEventBuffer events;
    const McGameChange changes = mc_game_step(&game, &events);
    assert(!mc_game_site_alive(&game, 0U));
    assert(game.battery_selection == McBatteryCenter);
    assert(mc_game_selected_battery(&game) == 3);
    assert(
        (changes & (McGameChangeSites | McGameChangeHud)) ==
        (McGameChangeSites | McGameChangeHud));
    assert(mc_game_fire(&game) == McFireSuccess);

    game.alive_site_mask &= (uint16_t) ~(1U << 3U);
    game.battery_ammo[1] = 0U;
    mc_game_rebuild_derived(&game);
    assert(game.battery_selection == McBatteryRight);
    assert(mc_game_selected_battery(&game) == 6);

    assert(!mc_game_select_battery(&game, McBatteryLeft));
    assert(!mc_game_select_battery(&game, McBatteryCenter));
    assert(game.battery_selection == McBatteryRight);
    assert(mc_game_select_battery(&game, McBatteryAuto));
    assert(game.battery_selection == McBatteryAuto);
    assert(mc_game_select_battery(&game, McBatteryRight));
    assert(game.battery_selection == McBatteryRight);
}

// Check acceleration thresholds, diagonal scaling, and clamping at both movement extremes
static void test_cursor_rates_and_bounds(void) {
    assert(mc_cursor_base_step_q8(McCursorSlow) == 115U);
    assert(mc_cursor_base_step_q8(McCursorNormal) == 173U);
    assert(mc_cursor_base_step_q8(McCursorFast) == 230U);
    assert(mc_cursor_step_q8(McCursorFast, 7U, false) == 230U);
    assert(mc_cursor_step_q8(McCursorFast, 8U, false) == 345U);
    assert(mc_cursor_step_q8(McCursorFast, 20U, false) == 460U);
    assert(mc_cursor_step_q8(McCursorFast, 20U, true) == 325U);

    McGame game;
    mc_game_init(&game);
    mc_game_move_cursor_q8(&game, INT16_MIN, INT16_MIN);
    assert(mc_game_cursor_x(&game) == MC_CURSOR_MIN_X);
    assert(mc_game_cursor_y(&game) == MC_CURSOR_MIN_Y);
    mc_game_move_cursor_q8(&game, INT16_MAX, INT16_MAX);
    assert(mc_game_cursor_x(&game) == MC_CURSOR_MAX_X);
    assert(mc_game_cursor_y(&game) == MC_CURSOR_MAX_Y);
}

// Verify cheap buffer reset and bounded event appends that flag overflow without overrunning
static void test_events_clear_metadata_only(void) {
    McGameEventBuffer events;
    memset(&events, 0xA5, sizeof(events));
    const McGameEvent first = events.items[0];
    mc_game_events_clear(&events);
    assert(events.count == 0U && !events.overflowed);
    assert(memcmp(&events.items[0], &first, sizeof(first)) == 0);
    for(uint8_t i = 0U; i < MC_GAME_EVENT_CAPACITY + 1U; i++) {
        mc_game_event_push(&events, McGameEventEnemyDestroyed, 1U);
    }
    assert(events.count == MC_GAME_EVENT_CAPACITY);
    assert(events.overflowed);
}

// Check that an eligible repair spends one credit and repeating it changes nothing
static void test_repair(void) {
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 99U);
    game.phase = McGamePhaseWaveResult;
    game.alive_site_mask &= (uint16_t) ~(1U << 1U);
    game.repair_credits = 1U;
    assert(mc_game_repair_site(&game, 1U));
    assert(mc_game_site_alive(&game, 1U));
    assert(game.repair_credits == 0U && game.stats.sites_repaired == 1U);
    assert(!mc_game_repair_site(&game, 1U));
    assert(game.repair_credits == 0U && game.stats.sites_repaired == 1U);
}

// Credit both overlapping shots while using the deepest chain for the intercepted missile
static void test_overlapping_root_credit_and_deepest_chain(void) {
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 123U);
    game.spawn_cooldown = 100U;
    game.roster_remaining[0] = 1U;
    game.roster_remaining[1] = 0U;
    game.roster_remaining[2] = 0U;
    game.pending_remaining = 1U;
    const McSiteDef* target = mc_game_site_def(1U);
    game.enemy_mask = 1U;
    game.enemy_count = 1U;
    game.enemies[0].kind = McEnemyStandard;
    game.enemies[0].target_site = 1U;
    mc_path_init(&game.enemies[0].path, target->x, target->y, target->x, target->y, 1U);
    // Two roots and a deeper chain cover the same target to exercise both crediting rules
    game.root_mask = 3U;
    game.root_count = 2U;
    for(uint8_t i = 0U; i < 2U; i++) {
        game.roots[i].x = target->x;
        game.roots[i].y = target->y;
        game.roots[i].max_radius = 9U;
        game.roots[i].radius = 9U;
    }
    game.chain_mask = 1U;
    game.chain_count = 1U;
    game.chains[0].x = target->x;
    game.chains[0].y = target->y;
    game.chains[0].max_radius = 6U;
    game.chains[0].radius = 6U;
    game.chains[0].chain_depth = 4U;
    McGameEventBuffer events;
    mc_game_step(&game, &events);
    assert(game.stats.successful_shots == 2U);
    assert(game.stats.max_chain_depth == 4U);
    assert(has_event(&events, McGameEventEnemyDestroyed));
    assert(game.enemy_count == 0U);
}

// Ensure a full impact pool replaces an old effect without taking defensive explosion slots
static void test_cosmetic_impact_pool_isolated(void) {
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 321U);
    game.spawn_cooldown = 100U;
    game.roster_remaining[0] = 1U;
    // Fill all effect pools so a cosmetic impact must recycle its own oldest slot
    game.pending_remaining = 1U;
    game.root_mask = UINT8_MAX;
    game.chain_mask = (1UL << MC_CHAIN_EXPLOSION_CAPACITY) - 1UL;
    game.impact_mask = (1U << MC_IMPACT_EFFECT_CAPACITY) - 1U;
    for(uint8_t i = 0U; i < MC_ROOT_EXPLOSION_CAPACITY; i++) {
        game.roots[i] = (McRootExplosion){
            .x = 127U,
            .y = MC_PLAYFIELD_TOP,
            .age = 1U,
            .max_radius = 1U,
            .radius = 1U,
        };
    }
    for(uint8_t i = 0U; i < MC_CHAIN_EXPLOSION_CAPACITY; i++) {
        game.chains[i] = (McChainExplosion){
            .x = 127U,
            .y = MC_PLAYFIELD_TOP,
            .age = 1U,
            .max_radius = 1U,
            .radius = 1U,
            .chain_depth = 1U,
        };
    }
    for(uint8_t i = 0U; i < MC_IMPACT_EFFECT_CAPACITY; i++) {
        game.impacts[i] = (McImpactEffect){
            .x = i,
            .y = MC_PLAYFIELD_TOP,
            .age = (uint8_t)(i + 1U),
            .max_radius = 4U,
            .radius = 1U,
        };
    }
    mc_game_rebuild_derived(&game);
    const uint8_t root_mask = game.root_mask;
    const uint32_t chain_mask = game.chain_mask;
    const McSiteDef* target = mc_game_site_def(1U);
    game.enemy_mask = 1U;
    game.enemy_count = 1U;
    game.enemies[0].kind = McEnemyStandard;
    game.enemies[0].target_site = 1U;
    mc_path_init(&game.enemies[0].path, target->x, target->y, target->x, target->y, 1U);
    McGameEventBuffer events;
    mc_game_step(&game, &events);
    assert(game.root_mask == root_mask);
    assert(game.chain_mask == chain_mask);
    assert(game.impact_count == MC_IMPACT_EFFECT_CAPACITY);
    assert(game.impacts[3].x == target->x && game.impacts[3].y == target->y);
}

// Round-trip preferences, retain older settings, and reject invalid or unsupported payloads.
static void test_settings_and_score_codecs(void) {
    McSettings settings;
    mc_settings_defaults(&settings);
    assert(!settings.invert_colors);
    settings.render_mode = McRenderBatterySaver;
    settings.invert_colors = true;
    uint8_t settings_data[MC_SETTINGS_ENCODED_SIZE];
    assert(
        mc_settings_encode(&settings, settings_data, sizeof(settings_data)) ==
        sizeof(settings_data));
    McSettings restored_settings;
    assert(mc_settings_decode(&restored_settings, settings_data, sizeof(settings_data)));
    assert(restored_settings.render_mode == McRenderBatterySaver);
    assert(restored_settings.invert_colors);
    // A version-2 fixture omits the appended flag but preserves all existing preferences.
    uint8_t legacy_settings[41];
    memcpy(legacy_settings, settings_data, sizeof(legacy_settings) - 4U);
    legacy_settings[4] = 2U;
    legacy_settings[6] = 29U;
    write_u32(legacy_settings + 37U, mc_crc32(legacy_settings, 37U));
    assert(mc_settings_decode(&restored_settings, legacy_settings, sizeof(legacy_settings)));
    assert(
        !restored_settings.invert_colors && restored_settings.render_mode == McRenderBatterySaver);
    assert(
        restored_settings.sound == settings.sound &&
        restored_settings.tap_pixels == settings.tap_pixels);
    // A valid checksum cannot make an out-of-range boolean acceptable.
    settings_data[37] = 2U;
    write_u32(settings_data + 38U, mc_crc32(settings_data, 38U));
    assert(!mc_settings_decode(&restored_settings, settings_data, sizeof(settings_data)));
    settings_data[37] = 1U;
    // Reseal the changed schema version so rejection tests compatibility rather than just CRC
    settings_data[4] = 4U;
    write_u32(
        settings_data + sizeof(settings_data) - 4U,
        mc_crc32(settings_data, sizeof(settings_data) - 4U));
    assert(!mc_settings_decode(&restored_settings, settings_data, sizeof(settings_data)));

    McScoreTables scores;
    mc_score_tables_defaults(&scores);
    McScoreEntry entry = {
        .score = 1200U,
        .wave = 3U,
        .difficulty = McDifficultyCadet,
        .stats = {.shots_fired = 20U, .successful_shots = 15U},
        .cities_remaining = 5U,
    };
    assert(mc_score_tables_insert(&scores, &entry));
    uint8_t score_data[MC_SCORES_ENCODED_SIZE];
    assert(mc_score_tables_encode(&scores, score_data, sizeof(score_data)) == sizeof(score_data));
    McScoreTables restored_scores;
    assert(mc_score_tables_decode(&restored_scores, score_data, sizeof(score_data)));
    assert(restored_scores.boards[McDifficultyCadet].entries[0].score == 1200U);
}

// Check record persistence, medal thresholds, and rejection just below the accuracy threshold
static void test_profile_medals_and_records(void) {
    McProfile profile;
    mc_profile_defaults(&profile);
    McGame game;
    mc_game_start_run(&game, McDifficultyCrisis, 42U);
    game.wave = 10U;
    game.score = 50000U;
    game.stats.shots_fired = 20U;
    game.stats.successful_shots = 15U;
    game.stats.max_chain_depth = 5U;
    game.stats.perfect_wave_streak = 5U;
    game.stats.best_perfect_wave_streak = 5U;
    game.battery_ammo[0] = 5U;
    game.battery_ammo[1] = 5U;
    game.battery_ammo[2] = 5U;
    const uint16_t wave_medals = mc_profile_update_wave(&profile, &game);
    assert(wave_medals & (1U << McMedalChainReaction));
    assert(wave_medals & (1U << McMedalPerfectDefense));
    assert(wave_medals & (1U << McMedalIronDome));
    assert(wave_medals & (1U << McMedalQuartermaster));
    assert(wave_medals & (1U << McMedalCrisisCommander));
    const uint16_t end_medals = mc_profile_update_game_over(&profile, &game);
    assert(end_medals & (1U << McMedalSharpshooter));
    assert(profile.best_accuracy_x100 == 7500U);
    assert(profile.best_score == 50000U);

    uint8_t encoded[MC_PROFILE_ENCODED_SIZE];
    assert(mc_profile_encode(&profile, encoded, sizeof(encoded)) == sizeof(encoded));
    McProfile restored;
    assert(mc_profile_decode(&restored, encoded, sizeof(encoded)));
    assert(restored.earned_medals == profile.earned_medals);

    mc_profile_defaults(&profile);
    game.stats.successful_shots = 14U;
    assert((mc_profile_update_game_over(&profile, &game) & (1U << McMedalSharpshooter)) == 0U);
}

// Verify snapshot compatibility, identical resumed simulation, and damaged-blob rejection
static void test_run_snapshot_round_trip_and_corruption(void) {
    McGame source_game;
    mc_game_start_run(&source_game, McDifficultyCrisis, 0xCAFEBABEU);
    source_game.battery_selection = McBatteryRight;
    mc_game_move_cursor_q8(&source_game, 321, -177);
    assert(mc_game_fire(&source_game) == McFireSuccess);
    for(uint8_t i = 0U; i < 20U; i++) {
        McGameEventBuffer events;
        mc_game_step(&source_game, &events);
    }
    McRunSnapshot source = {.game = &source_game, .stage = McRunStageActive};
    uint8_t encoded[MC_RUN_ENCODED_MAX_SIZE];
    const size_t size = mc_run_snapshot_encode(&source, encoded, sizeof(encoded));
    assert(size > 0U && size <= MC_RUN_ENCODED_MAX_SIZE);

    McGame restored_game;
    McRunSnapshot restored = {.game = &restored_game};
    assert(mc_run_snapshot_decode(&restored, encoded, size));
    assert(restored.stage == source.stage);
    assert(mc_game_state_hash(&restored_game) == mc_game_state_hash(&source_game));
    // This scenario was captured before removing trail ages. Only those two bytes
    // and the checksum may differ; older nonzero ages must still load correctly
    uint8_t previous[MC_RUN_ENCODED_MAX_SIZE];
    FILE* fixture = fopen("tests/fixtures/run_v1.dat", "rb");
    assert(fixture);
    const size_t previous_size = fread(previous, 1U, sizeof(previous), fixture);
    assert(!ferror(fixture) && previous_size == size);
    assert(fclose(fixture) == 0);
    assert(previous[111] == 20U && previous[126] == 20U);
    for(size_t i = 0U; i < size - 4U; i++)
        assert(encoded[i] == ((i == 111U || i == 126U) ? 0U : previous[i]));
    assert(mc_run_snapshot_decode(&restored, previous, previous_size));
    assert(restored.stage == source.stage);
    assert(mc_game_state_hash(&restored_game) == mc_game_state_hash(&source_game));
    for(uint8_t i = 0U; i < 30U; i++) {
        McGameEventBuffer source_events;
        McGameEventBuffer restored_events;
        mc_game_step(&source_game, &source_events);
        mc_game_step(&restored_game, &restored_events);
        assert(mc_game_state_hash(&restored_game) == mc_game_state_hash(&source_game));
    }
    encoded[20] ^= 0x80U;
    assert(!mc_run_snapshot_decode(&restored, encoded, size));
    assert(!mc_run_snapshot_decode(&restored, encoded, size - 1U));
}

// Check a crowded snapshot fits the budget and rejects resealed duplicate slot indices
static void test_maximum_run_codec(void) {
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 1U);
    game.enemy_mask = (1UL << 8U) - 1UL;
    game.interceptor_mask = UINT8_MAX;
    game.root_mask = UINT8_MAX;
    game.chain_mask = (1UL << MC_CHAIN_EXPLOSION_CAPACITY) - 1UL;
    game.impact_mask = (1U << MC_IMPACT_EFFECT_CAPACITY) - 1U;
    for(uint8_t i = 0U; i < 8U; i++) {
        game.enemies[i].kind = McEnemyStandard;
        game.enemies[i].target_site = i % MC_SITE_COUNT;
        const McSiteDef* target = mc_game_site_def(game.enemies[i].target_site);
        mc_path_init(&game.enemies[i].path, i, MC_PLAYFIELD_TOP, target->x, target->y, 30U);
    }
    for(uint8_t i = 0U; i < MC_INTERCEPTOR_CAPACITY; i++) {
        game.interceptors[i].battery_site = 0U;
        mc_path_init(&game.interceptors[i].path, 6, MC_GROUND_Y, 64, 30, 20U);
    }
    for(uint8_t i = 0U; i < MC_ROOT_EXPLOSION_CAPACITY; i++)
        game.roots[i].max_radius = 9U;
    for(uint8_t i = 0U; i < MC_CHAIN_EXPLOSION_CAPACITY; i++) {
        game.chains[i].max_radius = 6U;
        game.chains[i].chain_depth = 1U;
    }
    for(uint8_t i = 0U; i < MC_IMPACT_EFFECT_CAPACITY; i++)
        game.impacts[i].max_radius = 4U;
    mc_game_rebuild_derived(&game);
    assert(mc_game_validate(&game));
    McRunSnapshot run = {.game = &game, .stage = McRunStageActive};
    uint8_t encoded[MC_RUN_ENCODED_MAX_SIZE];
    const size_t size = mc_run_snapshot_encode(&run, encoded, sizeof(encoded));
    assert(size > 0U && size <= MC_RUN_ENCODED_MAX_SIZE);
    McGame decoded_game;
    McRunSnapshot decoded = {.game = &decoded_game};
    assert(mc_run_snapshot_decode(&decoded, encoded, size));
    // Duplicate the first enemy slot in the next record, then reseal to test structural checks
    encoded[8U + 86U + 18U] = 0U;
    write_u32(encoded + size - 4U, mc_crc32(encoded, size - 4U));
    assert(!mc_run_snapshot_decode(&decoded, encoded, size));
}

// Run the compact deterministic gameplay, geometry, memory, and codec checks
// Golden traces from the pre-refactor engine cover every mode and difficulty.
static unsigned trace(unsigned mode, unsigned difficulty) {
    McGame g;
    unsigned hash = 2166136261U;
    mc_game_start_mode(&g, difficulty, 0x13579BDFU, mode);
    for(unsigned tick = 0; tick < 600; tick++) {
        if(g.phase == McGamePhaseWaveResult) mc_game_begin_next_wave(&g);
        if(tick % 11 == 0) {
            mc_game_move_cursor_q8(
                &g, (tick % 3 == 0 ? -3 : 2) * 256, (tick % 5 == 0 ? -2 : 1) * 256);
            mc_game_fire(&g);
        }
        McGameEventBuffer events;
        mc_game_step(&g, &events);
        hash = (hash ^ mc_game_state_hash(&g)) * 16777619U;
    }
    return hash;
}
static void test_mode_policy_parity(void) {
    assert(mc_mode_def(McModeCount) == mc_mode_def(McModeClassic));
    assert(mc_mode_def(UINT8_MAX) == mc_mode_def(McModeClassic));
    for(unsigned mode = 0; mode < McModeCount; mode++) {
        const McModeDef* retained = mc_mode_def(mode);
        const McModeDef expected = *retained;
        for(unsigned next = 0; next < McModeCount; next++)
            (void)mc_mode_def(next);
        assert(!memcmp(retained, &expected, sizeof(expected)));
    }
    static const uint32_t expected[McModeCount * McDifficultyCount] = {
        3754582404U, 1383945587U, 1787849081U, 706502963U,  706502963U,  706502963U,  2923985188U,
        247389851U,  925200717U,  3074707444U, 3533076651U, 4098237099U, 693116453U,  250966364U,
        150135480U,  2759280223U, 308982449U,  3981732867U, 2793401889U, 2519829943U, 4203584176U,
        2598762615U, 2598762615U, 2598762615U, 1617754491U, 1617754491U, 1617754491U, 3842532916U,
        4059740707U, 1321254209U, 696362436U,  2755612683U, 2979673501U, 2244247860U, 50919635U,
        2503842069U, 2176888965U, 1647114653U, 3392348260U, 2235768316U, 4007149571U, 1168690801U,
        352293001U,  848096631U,  3244700215U,
    };
    for(unsigned m = 0; m < McModeCount; m++)
        for(unsigned d = 0; d < McDifficultyCount; d++)
            assert(trace(m, d) == expected[m * McDifficultyCount + d]);
}
static void test_exact_wave_replay(void) {
    for(unsigned mode = 0; mode < McModeCount; mode++) {
        if(!mc_mode_has(mode, McModeReplay)) continue;
        McGame original, replay;
        mc_game_start_mode(&original, McDifficultyCrisis, 0xDEADBEEFU, mode);
        original.wave = 23;
        original.phase = McGamePhaseWaveResult;
        original.next_supplies = 3;
        original.next_radius_boost = 2;
        original.bonus_ammo = 3;
        mc_game_begin_next_wave(&original);
        McWaveStart start;
        assert(mc_wave_start_capture(&start, &original));
        assert(start.size <= MC_WAVE_START_MAX_SIZE);
        assert(mc_wave_start_restore(&start, &replay));
        assert(mc_game_state_hash(&original) == mc_game_state_hash(&replay));
        for(unsigned tick = 0; tick < 300; tick++) {
            if(tick % 17 == 0) {
                mc_game_fire(&original);
                mc_game_fire(&replay);
            }
            McGameEventBuffer a, b;
            assert(mc_game_step(&original, &a) == mc_game_step(&replay, &b));
            assert(a.count == b.count);
            assert(mc_game_state_hash(&original) == mc_game_state_hash(&replay));
        }
        start.data[start.size - 1] ^= 1;
        assert(!mc_wave_start_restore(&start, &replay));
    }
}

// Fixed byte patterns and independently known CRCs cover unaligned binary buffers.
static void test_binary_and_masks(void) {
    uint8_t bytes[9] = {0xA5, 0xEF, 0xCD, 0xAB, 0x89, 0x34, 0x12, 0xA5, 0xA5};
    assert(mc_read_u32(bytes + 1) == 0x89ABCDEFU);
    assert(mc_read_u16(bytes + 5) == 0x1234U);
    mc_write_u32(bytes + 1, 0x76543210U);
    mc_write_u16(bytes + 5, 0xFEDCU);
    const uint8_t expected[] = {0xA5, 0x10, 0x32, 0x54, 0x76, 0xDC, 0xFE, 0xA5, 0xA5};
    assert(!memcmp(bytes, expected, sizeof(bytes)));
    assert(mc_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926U);
    assert(mc_crc32(bytes, 0) == 0U);
    assert(mc_popcount32(0) == 0 && mc_popcount32(UINT32_MAX) == 32);
    assert(mc_popcount32(UINT8_MAX) == 8 && mc_popcount32(UINT16_MAX) == 16);
    uint32_t mask = 0x80008181U;
    const uint8_t indices[] = {0, 7, 8, 15, 31};
    for(unsigned i = 0; i < sizeof(indices); i++)
        assert(mc_mask_take_first(&mask) == indices[i]);
    assert(mask == 0);
    assert(mc_mask_first(0) == -1);
    assert(mc_mask_first(0x80000000U) == 31);
    assert(mc_mask_first(UINT32_MAX) == 0);
    assert(mc_mask_first(0x8000U) == 15);
    assert(mc_mask_first(0x80U) == 7);
}

// Each public step must retain the old radius curve, expiry timing, and duration supply.
static void test_effect_boundaries(void) {
    const uint8_t ages[] = {
        0, 7, 8, 12, 13, MC_EXPLOSION_TOTAL_TICKS - 2, MC_EXPLOSION_TOTAL_TICKS - 1};
    for(unsigned supply = 0; supply < 2; supply++) {
        for(unsigned parity = 0; parity < 2; parity++) {
            for(unsigned a = 0; a < sizeof(ages); a++) {
                McGame g;
                mc_game_start_run(&g, McDifficultyCommand, 19);
                g.spawn_cooldown = 100;
                g.supplies = supply ? 2 : 0;
                g.stats.play_ticks = parity;
                g.root_mask = g.chain_mask = g.impact_mask = 1;
                g.roots[0] = (McRootExplosion){.age = ages[a], .max_radius = 8};
                g.chains[0] =
                    (McChainExplosion){.age = ages[a], .max_radius = 8, .chain_depth = 3};
                g.impacts[0] = (McImpactEffect){.age = ages[a], .max_radius = 8};
                mc_game_rebuild_derived(&g);
                assert(g.roots[0].age == ages[a] && g.chains[0].age == ages[a]);
                assert(
                    g.roots[0].radius == g.chains[0].radius &&
                    g.roots[0].radius == g.impacts[0].radius);
                mc_game_step(&g, NULL);
                // The tick increments before effects; only even ticks hold defensive blasts.
                const uint8_t age = ages[a] + !(supply && parity && ages[a] >= 8 && ages[a] <= 12);
                assert(g.roots[0].age == age && g.chains[0].age == age);
                assert(g.impacts[0].age == ages[a] + 1);
                assert(g.root_count == (age < MC_EXPLOSION_TOTAL_TICKS));
                assert(
                    g.chain_count == g.root_count && g.root_mask == g.root_count &&
                    g.chain_mask == g.chain_count);
                assert(g.impact_count == (ages[a] + 1U < MC_EXPLOSION_TOTAL_TICKS));
                assert(g.impact_mask == g.impact_count);
                assert(g.chains[0].chain_depth == 3);
            }
        }
    }
}

// Real interceptor arrivals and enemy collisions exercise replacement in all typed pools.
static void test_effect_slot_order(void) {
    for(unsigned pool = 0; pool < 3; pool++) {
        for(unsigned gap = 0; gap < 2; gap++) {
            McGame g;
            mc_game_start_run(&g, McDifficultyCommand, 21);
            g.spawn_cooldown = 100;
            const uint8_t capacity = pool == 0 ? MC_ROOT_EXPLOSION_CAPACITY :
                                     pool == 1 ? MC_CHAIN_EXPLOSION_CAPACITY :
                                                 MC_IMPACT_EFFECT_CAPACITY;
            const uint32_t mask = ((1UL << capacity) - 1U) & ~(gap ? 1UL << 2 : 0);
            const unsigned chosen = gap ? 2 : 0; // All occupied ages tie.
            for(unsigned i = 0; i < capacity; i++) {
                if(pool == 0)
                    g.roots[i] = (McRootExplosion){.x = 127, .y = 8, .age = 4, .max_radius = 1};
                if(pool == 1)
                    g.chains[i] = (McChainExplosion){
                        .x = 127, .y = 8, .age = 4, .max_radius = 1, .chain_depth = 2};
                if(pool == 2)
                    g.impacts[i] = (McImpactEffect){.x = 127, .y = 8, .age = 4, .max_radius = 1};
            }
            if(pool == 0) {
                g.root_mask = mask;
                g.interceptor_mask = 1;
                g.interceptors[0].battery_site = 0;
                mc_path_init(&g.interceptors[0].path, 64, 24, 64, 24, 1);
            } else {
                if(pool == 1) {
                    g.chain_mask = mask;
                    g.root_mask = 1;
                    g.roots[0] = (McRootExplosion){.x = 64, .y = 24, .max_radius = 8, .age = 8};
                    mc_path_init(&g.enemies[0].path, 64, 24, 64, 24, 1);
                } else {
                    g.impact_mask = mask;
                    const McSiteDef* site = mc_game_site_def(1);
                    mc_path_init(&g.enemies[0].path, site->x, site->y, site->x, site->y, 1);
                }
                g.enemies[0].target_site = 1;
                g.enemies[0].kind = McEnemyStandard;
                g.enemy_mask = 1;
            }
            mc_game_rebuild_derived(&g);
            mc_game_step(&g, NULL);
            assert(
                (pool == 0 ? g.root_count :
                 pool == 1 ? g.chain_count :
                             g.impact_count) == capacity);
            for(unsigned i = 0; i < capacity; i++) {
                const uint8_t age = pool == 0 ? g.roots[i].age :
                                    pool == 1 ? g.chains[i].age :
                                                g.impacts[i].age;
                assert(age == (i == chosen ? 1 : 5));
            }
        }
    }
}

static void test_progression_contexts(void) {
    const uint16_t crisis = 1U << McMedalCrisisCommander;
    for(unsigned wave = 9; wave <= 11; wave++) {
        McGame g;
        mc_game_start_run(&g, McDifficultyCrisis, 23);
        g.wave = wave;
        g.score = 1234;
        g.stats.shots_fired = 20;
        g.stats.successful_shots = 15;
        McProfile p;
        mc_profile_defaults(&p);
        assert(!(mc_profile_update_started(&p, &g) & crisis));
        assert(p.best_score == 0 && p.best_accuracy_x100 == 0 && p.best_wave == wave);
        assert(mc_profile_update_started(&p, &g) == 0);
        assert(!!(mc_profile_update_wave(&p, &g) & crisis) == (wave >= 10));
        assert(p.best_score == 1234 && p.best_accuracy_x100 == 0);
        assert(mc_profile_update_wave(&p, &g) == 0);
        mc_profile_defaults(&p);
        assert(!!(mc_profile_update_game_over(&p, &g) & crisis) == (wave >= 11));
        assert(p.best_accuracy_x100 == 7500);
        assert(mc_profile_update_game_over(&p, &g) == 0);
    }
    for(unsigned mode = 0; mode < McModeCount; mode++) {
        for(unsigned difficulty = 0; difficulty < McDifficultyCount; difficulty++) {
            assert(!mc_session_has(mode, true, McModeSave));
            assert(!mc_session_has(mode, true, McModeRanked));
            assert(!mc_session_has(mode, true, McModeCompetitive));
            assert(mc_session_has(mode, true, McModeReplay) == mc_mode_has(mode, McModeReplay));
            assert(mc_session_has(mode, false, McModeSave) == mc_mode_has(mode, McModeSave));
            McGame g;
            mc_game_start_mode(&g, difficulty, 23, mode);
            assert(g.difficulty == mc_mode_difficulty(mode, difficulty));
        }
    }
}

int main(void) {
    test_binary_and_masks();
    test_effect_boundaries();
    test_effect_slot_order();
    test_progression_contexts();
    test_mode_policy_parity();
    test_exact_wave_replay();
    test_structure_budgets();
    test_mode_startup();
    test_rational_scheduler_and_render_modes();
    test_q9_path_exact_arrival();
    test_weighted_roster_and_modifiers();
    test_director_rng_isolated();
    test_deterministic_simulation();
    test_masks_and_battery_lock();
    test_destroyed_selected_battery_falls_back();
    test_cursor_rates_and_bounds();
    test_events_clear_metadata_only();
    test_repair();
    test_overlapping_root_credit_and_deepest_chain();
    test_cosmetic_impact_pool_isolated();
    test_settings_and_score_codecs();
    test_profile_medals_and_records();
    test_run_snapshot_round_trip_and_corruption();
    test_maximum_run_codec();
    puts("core tests passed");
    return 0;
}
