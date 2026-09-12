// Golden bytes captured from the pre-consolidation source snapshot on 2026-09-11.
// MC_CAPTURE_BASELINE is only for recording that preserved engine, never normal tests.
#include "game.h"
#include "persistence_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static FILE* fixture;
static void check(const uint8_t* bytes, size_t size) {
#ifdef MC_CAPTURE_BASELINE
    assert(fwrite(bytes, 1, size, fixture) == size);
#else
    for(size_t i = 0; i < size; i++)
        assert(fgetc(fixture) == bytes[i]);
#endif
}
static void scalar(uint32_t value) {
    const uint8_t bytes[] = {value, value >> 8, value >> 16, value >> 24};
    check(bytes, sizeof(bytes));
}
static void snapshot(McGame* game) {
    uint8_t encoded[MC_RUN_ENCODED_MAX_SIZE];
    McRunSnapshot run = {.game = game, .stage = McRunStageActive};
    size_t size = mc_run_snapshot_encode(&run, encoded, sizeof(encoded));
    assert(size);
    scalar(size);
    check(encoded, size);
    scalar(mc_game_state_hash(game));
}

int main(int argc, char** argv) {
    assert(argc == 2);
#ifdef MC_CAPTURE_BASELINE
    fixture = fopen(argv[1], "wb");
#else
    fixture = fopen(argv[1], "rb");
#endif
    assert(fixture);
    McSettings settings;
    mc_settings_defaults(&settings);
    settings.resume_countdown = false; // Preserve the preference encoded in the old fixture.
    settings.last_setup_valid = true;
    settings.last_mode = McModePractice;
    settings.last_seed = 0x89ABCDEFU;
    settings.last_difficulty = McDifficultyCrisis;
    settings.last_options =
        (McRunOptions){.start_wave = 65534, .enemy = 4, .slow = 1, .unlimited = 1};
    settings.invert_colors = true;
    uint8_t settings_bytes[MC_SETTINGS_ENCODED_SIZE];
    assert(
        mc_settings_encode(&settings, settings_bytes, sizeof(settings_bytes)) ==
        sizeof(settings_bytes));
    check(settings_bytes, sizeof(settings_bytes));
    McScoreTables scores;
    mc_score_tables_defaults(&scores);
    for(unsigned difficulty = 0; difficulty < McDifficultyCount; difficulty++) {
        for(unsigned i = 0; i < MC_SCORE_CAPACITY; i++) {
            McScoreEntry entry = {
                .difficulty = difficulty,
                .score = (i + 1) * 1001,
                .timestamp = 0x12345678U + i,
                .wave = i + 10,
                .cities_remaining = 3,
                .stats = {
                    .shots_fired = 100 + i,
                    .successful_shots = 77,
                    .enemies_destroyed = 87,
                    .play_ticks = 12345,
                    .sites_lost = 3,
                    .sites_repaired = 2,
                    .max_chain_depth = 5}};
            assert(mc_score_tables_insert(&scores, &entry));
        }
    }
    uint8_t scores_bytes[MC_SCORES_ENCODED_SIZE];
    assert(
        mc_score_tables_encode(&scores, scores_bytes, sizeof(scores_bytes)) ==
        sizeof(scores_bytes));
    check(scores_bytes, sizeof(scores_bytes));
    McProfile profile;
    mc_profile_defaults(&profile);
    for(unsigned mode = 0; mode < McModeCount; mode++) {
        for(unsigned difficulty = 0; difficulty < McDifficultyCount; difficulty++) {
            McGame game;
            mc_game_start_mode(&game, difficulty, 0x13579BDFU, mode);
            McWaveStart wave;
            if(mc_wave_start_capture(&wave, &game)) {
                scalar(wave.size);
                check(wave.data, wave.size);
            } else
                scalar(0);
            snapshot(&game);
            for(unsigned tick = 0; tick < 600; tick++) {
                if(game.phase == McGamePhaseWaveResult) {
                    mc_profile_update_wave(&profile, &game);
                    mc_game_begin_next_wave(&game);
                }
                if(tick % 11 == 0) {
                    mc_game_move_cursor_q8(
                        &game, (tick % 3 == 0 ? -3 : 2) * 256, (tick % 5 == 0 ? -2 : 1) * 256);
                    mc_game_fire(&game);
                }
                McGameEventBuffer events;
                mc_game_step(&game, &events);
                if(tick == 179 || tick == 599) snapshot(&game);
            }
            mc_profile_update_game_over(&profile, &game);
        }
    }
    uint8_t profile_bytes[MC_PROFILE_ENCODED_SIZE];
    assert(
        mc_profile_encode(&profile, profile_bytes, sizeof(profile_bytes)) ==
        sizeof(profile_bytes));
    check(profile_bytes, sizeof(profile_bytes));
#ifndef MC_CAPTURE_BASELINE
    assert(fgetc(fixture) == EOF);
#endif
    assert(fclose(fixture) == 0);
    puts("pre-refactor settings, scores, profiles, replay data and run bytes match");
    return 0;
}
