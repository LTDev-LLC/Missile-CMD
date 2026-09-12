#include "binary.h"
#pragma once

#include "game_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_SCORE_CAPACITY        5U
#define MC_SETTINGS_ENCODED_SIZE 42U
#define MC_SCORES_ENCODED_SIZE   507U
#define MC_PROFILE_ENCODED_SIZE  (37U + McModeCount * 4U + McDifficultyCount * 2U)
#define MC_RUN_ENCODED_MAX_SIZE  1024U

typedef uint8_t McCursorSpeed;
enum {
    McCursorSlow = 0,
    McCursorNormal,
    McCursorFast,
    McCursorCount,
};

typedef uint8_t McRenderMode;
enum {
    McRenderAdaptive = 0,
    McRenderBatterySaver,
    McRenderModeCount,
};

typedef struct {
    McCursorSpeed cursor_speed;
    McDifficulty difficulty;
    McRenderMode render_mode;
    bool sound;
    bool vibration;
    bool show_control_card;
    bool reduced_flash;
    uint8_t trail_density; // current wire values: 0 off, 2 on
    bool cursor_acceleration;
    bool strong_cursor;
    bool invert_colors;
    uint8_t vibration_intensity; // 0 light, 1 normal
    uint8_t led_mode; // all, critical only, off
    bool simple_hud;
    bool resume_countdown;
    uint8_t tap_pixels; // 1..3, independent of held movement
    bool practice_aids;
    bool impact_warnings;
    bool last_setup_valid;
    McGameMode last_mode;
    McDifficulty last_difficulty;
    uint32_t last_seed;
    McRunOptions last_options;
} McSettings;

typedef struct {
    uint32_t shots_fired;
    uint32_t successful_shots;
    uint32_t enemies_destroyed;
    uint32_t play_ticks;
    uint16_t sites_lost;
    uint16_t sites_repaired;
    uint8_t max_chain_depth;
} __attribute__((packed)) McScoreStats;

typedef struct {
    uint32_t score;
    uint32_t timestamp;
    McScoreStats stats;
    uint16_t wave;
    McDifficulty difficulty;
    uint8_t cities_remaining;
} McScoreEntry;

typedef struct {
    McScoreEntry entries[MC_SCORE_CAPACITY];
} McScoreBoard;

typedef struct {
    McScoreBoard boards[McDifficultyCount];
} McScoreTables;

typedef uint8_t McMedal;
enum {
    McMedalChainReaction = 0,
    McMedalSharpshooter,
    McMedalPerfectDefense,
    McMedalIronDome,
    McMedalQuartermaster,
    McMedalSurvivor,
    McMedalCrisisCommander,
    McMedalLastStand,
    McMedalDailyDuty,
    McMedalEconomist,
    McMedalLoneCity,
    McMedalStormKeeper,
    McMedalFlawless,
    McMedalCrisisVeteran,
    McMedalCadetGraduate,
    McMedalCount,
};

typedef struct {
    uint32_t best_score;
    uint32_t best_survival_ticks;
    uint16_t best_wave;
    uint16_t best_accuracy_x100;
    uint16_t earned_medals;
    uint8_t best_chain;
    uint8_t best_perfect_streak;
    uint32_t mode_best[McModeCount];
    uint32_t daily_seed;
    uint32_t daily_best;
    uint8_t pinned_medal; // UINT8_MAX means no pinned goal
    uint16_t puzzles_completed[McDifficultyCount];
} McProfile;

typedef uint8_t McRunStage;
enum {
    McRunStageActive = 0,
    McRunStageWaveResult,
    McRunStageRepair,
    McRunStageCount,
};

// The caller owns game; decode into scratch state before publishing it live
#define MC_WAVE_START_MAX_SIZE 256U
typedef struct {
    uint16_t size; // zero means unavailable, including checkpoints made by older versions
    uint8_t data[MC_WAVE_START_MAX_SIZE];
} McWaveStart;

typedef struct {
    McGame* game;
    McRunStage stage;
    McWaveStart* wave_start;
} McRunSnapshot;

bool mc_wave_start_capture(McWaveStart* start, const McGame* game);
bool mc_wave_start_restore(const McWaveStart* start, McGame* game);
bool mc_wave_start_matches(const McWaveStart* start, const McGame* game);

void mc_settings_defaults(McSettings* settings);
void mc_score_tables_defaults(McScoreTables* scores);
// Equal scores retain their insertion order
bool mc_score_tables_insert(McScoreTables* scores, const McScoreEntry* entry);
const McScoreBoard* mc_score_board(const McScoreTables* scores, McDifficulty difficulty);
void mc_profile_defaults(McProfile* profile);
uint16_t mc_accuracy_x100(uint32_t hits, uint32_t shots);
// Returns only newly awarded medal bits
uint16_t mc_profile_update_wave(McProfile* profile, const McGame* game);
uint16_t mc_profile_update_game_over(McProfile* profile, const McGame* game);
const char* mc_medal_name(McMedal medal);

size_t mc_settings_encode(const McSettings* settings, uint8_t* output, size_t capacity);
bool mc_settings_decode(McSettings* settings, const uint8_t* input, size_t size);
size_t mc_score_tables_encode(const McScoreTables* scores, uint8_t* output, size_t capacity);
bool mc_score_tables_decode(McScoreTables* scores, const uint8_t* input, size_t size);
size_t mc_profile_encode(const McProfile* profile, uint8_t* output, size_t capacity);
bool mc_profile_decode(McProfile* profile, const uint8_t* input, size_t size);
size_t mc_run_snapshot_encode(const McRunSnapshot* run, uint8_t* output, size_t capacity);
// Decoding may modify game on failure; publish it only after a successful return
bool mc_run_snapshot_decode(McRunSnapshot* run, const uint8_t* input, size_t size);

_Static_assert(MC_RUN_ENCODED_MAX_SIZE <= 1024U, "run encoding exceeds budget");
_Static_assert(sizeof(McScoreTables) <= 552U, "score tables exceed encoded-size budget");
_Static_assert(McMedalCount <= 16U, "medals must fit the persisted mask");

void mc_medal_progress(
    const McProfile* profile,
    const McGame* game,
    uint8_t medal,
    uint16_t* value,
    uint16_t* target);

#define MC_PACE_ENCODED_SIZE (16U + MC_PACE_WAVES * 4U)
// Unrecorded waves use zero; recorded scores cannot exceed the final score
typedef struct {
    uint32_t score;
    uint32_t waves[MC_PACE_WAVES];
} McPace;
size_t mc_pace_encode(const McPace* pace, uint8_t* output, size_t capacity);
bool mc_pace_decode(McPace* pace, const uint8_t* input, size_t size);

typedef enum {
    McProfileWaveStarted,
    McProfileWaveCleared,
    McProfileRunEnded
} McProfileEvent;
uint16_t mc_profile_update_started(McProfile* profile, const McGame* game);
