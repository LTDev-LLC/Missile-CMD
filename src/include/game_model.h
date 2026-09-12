#pragma once

#include "binary.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_SCREEN_WIDTH     128
#define MC_SCREEN_HEIGHT    64
#define MC_PLAYFIELD_TOP    9
#define MC_PLAYFIELD_BOTTOM 50
#define MC_GROUND_Y         52
#define MC_CURSOR_HALF_SIZE 3
#define MC_CURSOR_MIN_X     MC_CURSOR_HALF_SIZE
// Bounds include room for the complete crosshair
#define MC_CURSOR_MAX_X     (MC_SCREEN_WIDTH - 1 - MC_CURSOR_HALF_SIZE)
#define MC_CURSOR_MIN_Y     (MC_PLAYFIELD_TOP + MC_CURSOR_HALF_SIZE)
#define MC_CURSOR_MAX_Y     (MC_PLAYFIELD_BOTTOM - MC_CURSOR_HALF_SIZE)

#define MC_SITE_COUNT               9U
#define MC_CITY_COUNT               6U
#define MC_BATTERY_COUNT            3U
#define MC_ENEMY_CAPACITY           20U
#define MC_INTERCEPTOR_CAPACITY     8U
#define MC_ROOT_EXPLOSION_CAPACITY  8U
#define MC_CHAIN_EXPLOSION_CAPACITY 20U
#define MC_IMPACT_EFFECT_CAPACITY   4U
#define MC_PENDING_CAPACITY         24U
#define MC_AMMO_PER_BATTERY         10U
#define MC_GAME_EVENT_CAPACITY      40U
#define MC_REPAIR_CREDIT_SCORE      10000UL
#define MC_REPAIR_CREDIT_CAP        2U
// Q9 trajectories use 512 units per pixel; Q8 cursor motion uses 256
#define MC_PATH_SHIFT               9U
#define MC_CURSOR_SHIFT             8U
#define MC_CURSOR_ONE               (1U << MC_CURSOR_SHIFT)
#define MC_ALL_SITE_MASK            ((1U << MC_SITE_COUNT) - 1U)

// These byte-sized enums are persisted; preserve their order across save schemas
typedef uint8_t McDifficulty;
enum {
    McDifficultyCadet = 0,
    McDifficultyCommand,
    McDifficultyCrisis,
    McDifficultyCount,
};

typedef uint8_t McGamePhase;
enum {
    McGamePhaseIdle = 0,
    McGamePhasePlaying,
    McGamePhaseWaveResult,
    McGamePhaseGameOver,
};

typedef uint8_t McGameMode;
enum {
    McModeClassic = 0,
    McModeDaily,
    McModeSeeded,
    McModeLimitedAmmo,
    McModeOneCity,
    McModeBarrage,
    McModePerfect,
    McModeCrisis,
    McModeTraining,
    McModePractice,
    McModeCampaign,
    McModeSprint,
    McModePuzzle,
    McModeDuel,
    McModeEvasive,
    McModeCount,
};

typedef uint8_t McSupply;
enum {
    McSupplyAmmo = 0,
    McSupplyRadius,
    McSupplySpeed,
    McSupplyDuration,
    McSupplyCount
};

typedef uint8_t McSiteKind;
enum {
    McSiteCity = 0,
    McSiteBattery,
};

typedef uint8_t McEnemyKind;
enum {
    McEnemyStandard = 0,
    McEnemyFast,
    McEnemySplitter,
    McEnemySplitChild,
    McEnemyEvasive,
    McEnemyKindCount,
};

typedef uint8_t McWavePattern;
enum {
    McWavePatternStaggered = 0,
    McWavePatternSalvo,
    McWavePatternFlanks,
    McWavePatternSweep,
    McWavePatternConverge,
    McWavePatternGroups,
    McWavePatternCount,
};

typedef uint8_t McWaveModifier;
enum {
    McWaveModifierNone = 0,
    McWaveModifierBarrage,
    McWaveModifierVelocity,
    McWaveModifierFastSwarm,
    McWaveModifierFracture,
    McWaveModifierFocusFire,
    McWaveModifierOverload,
    McWaveModifierCount,
};

typedef uint8_t McBatterySelection;
enum {
    McBatteryAuto = 0,
    McBatteryLeft,
    McBatteryCenter,
    McBatteryRight,
    McBatterySelectionCount,
};

typedef uint8_t McFireResult;
enum {
    McFireSuccess = 0,
    McFireNotPlaying,
    McFireNoBattery,
    McFirePoolFull,
    McFireEmpty,
};

// Redraw flags accumulate until the application publishes a frame
typedef uint16_t McGameChange;
enum {
    McGameChangeNone = 0U,
    McGameChangeCursor = 1U << 0,
    McGameChangeHud = 1U << 1,
    McGameChangeObjects = 1U << 2,
    McGameChangeEffects = 1U << 3,
    McGameChangeSites = 1U << 4,
    McGameChangePhase = 1U << 5,
    McGameChangeAll = 0xFFFFU,
};

typedef struct {
    McSiteKind kind;
    uint8_t x;
    uint8_t y;
} McSiteDef;

typedef struct {
    uint16_t x_q9;
    uint16_t y_q9;
    int16_t vx_q9;
    int16_t vy_q9;
    uint16_t remaining;
    uint8_t target_x;
    uint8_t target_y;
} McPath;

typedef struct {
    McPath path;
    // Split when remaining flight ticks reach this threshold
    uint16_t split_remaining;
    uint8_t target_site;
    McEnemyKind kind;
    uint8_t origin_x, origin_y;
} McEnemyMissile;

typedef struct {
    McPath path;
    uint8_t battery_site;
} McInterceptor;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t age;
    uint8_t max_radius;
    uint8_t radius;
    uint8_t flags;
} McRootExplosion;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t age;
    uint8_t max_radius;
    uint8_t radius;
    uint8_t chain_depth;
} McChainExplosion;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t age;
    uint8_t max_radius;
    uint8_t radius;
} McImpactEffect;

#define MC_ROOT_HIT_CREDITED (1U << 0)

typedef struct {
    uint32_t shots_fired;
    uint32_t successful_shots;
    uint32_t enemies_destroyed;
    uint32_t play_ticks;
    uint16_t sites_lost;
    uint16_t sites_repaired;
    uint8_t max_chain_depth;
    uint8_t perfect_wave_streak;
    uint8_t best_perfect_wave_streak;
} McRunStats;

typedef uint8_t McGameEventType;
enum {
    McGameEventEnemyDestroyed = 0,
    McGameEventSiteLost,
    McGameEventWaveCleared,
    McGameEventGameOver,
};

typedef struct {
    McGameEventType type;
    uint8_t chain_depth;
} McGameEvent;

typedef struct {
    McGameEvent items[MC_GAME_EVENT_CAPACITY];
    uint8_t count;
    bool overflowed;
    // Coalesce cosmetic effects without using scored-event slots
    bool explosion;
} McGameEventBuffer;

typedef struct {
    uint16_t spawn_interval_ticks;
    uint16_t enemy_total;
    uint8_t max_active_enemies;
    uint8_t defensive_radius;
    uint16_t kind_speed_x100[McEnemyKindCount];
} McWaveBalance;

#define MC_PACE_WAVES     32U
#define MC_RULES_VERSION  2U
#define MC_PUZZLE_COUNT   9U
#define MC_CAMPAIGN_WAVES 5U
#define MC_SPRINT_TICKS   (3UL * 60U * 30U)
typedef struct {
    uint16_t start_wave; // zero is wave 1; Practice 0..65534, Puzzle 0..8
    uint8_t enemy; // Practice: mixed, standard, fast, splitter, evasive
    uint8_t slow;
    uint8_t unlimited;
} McRunOptions;

typedef struct {
    uint16_t shots;
    uint16_t hits;
    uint16_t kills;
    uint8_t chain;
} McWaveStats;

typedef struct {
    McEnemyMissile enemies[MC_ENEMY_CAPACITY];
    McInterceptor interceptors[MC_INTERCEPTOR_CAPACITY];
    McRootExplosion roots[MC_ROOT_EXPLOSION_CAPACITY];
    McChainExplosion chains[MC_CHAIN_EXPLOSION_CAPACITY];
    McImpactEffect impacts[MC_IMPACT_EFFECT_CAPACITY];
    McRunStats stats;
    McWaveBalance balance;

    uint32_t enemy_mask;
    uint32_t chain_mask;
    uint32_t score;
    uint32_t rng_state;
    uint32_t roster_rng_state;
    uint32_t director_rng_state;
    uint32_t next_repair_score;
    uint32_t seed;

    uint16_t alive_site_mask;
    uint16_t wave_start_alive_mask;
    uint16_t cursor_x_q8;
    uint16_t cursor_y_q8;
    uint16_t wave;
    uint16_t spawn_cooldown;
    uint16_t last_wave_bonus;

    uint8_t interceptor_mask;
    uint8_t root_mask;
    uint8_t impact_mask;
    uint8_t enemy_count;
    uint8_t interceptor_count;
    uint8_t root_count;
    uint8_t chain_count;
    uint8_t impact_count;

    uint8_t battery_ammo[MC_BATTERY_COUNT];
    uint8_t roster_remaining[3];
    uint8_t pending_remaining;
    McGamePhase phase;
    McDifficulty difficulty;
    McWavePattern wave_pattern;
    McWaveModifier modifier;
    McWaveModifier next_modifier;
    McBatterySelection battery_selection;
    uint8_t priority_site;
    uint8_t pattern_spawn_index;
    uint8_t repair_credits;
    uint8_t last_spawn_x;
    McGameMode mode;
    uint8_t bonus_ammo;
    uint8_t radius_boost;
    uint8_t next_radius_boost;
    uint8_t training_flags;
    McRunOptions options;
    McWaveStats wave_stats;
    uint32_t wave_scores[MC_PACE_WAVES];
    // Rules version is independent of the save schema and data-folder version
    uint8_t rules_version;
    // Bits 0/1 enable speed/duration; queued supplies activate next wave
    uint8_t supplies;
    uint8_t next_supplies;
    uint8_t last_lost_site;
    McEnemyKind last_lost_enemy;
    bool puzzle_failed;
    bool victory;
} McGame;

_Static_assert(sizeof(McPath) == 12U, "McPath must remain compact");
_Static_assert(sizeof(McGameEvent) == 2U, "McGameEvent must remain two bytes");
_Static_assert(sizeof(McGameEventBuffer) <= 96U, "McGameEventBuffer exceeds budget");
_Static_assert(sizeof(McGame) <= 1024U, "McGame exceeds 1 KiB budget");
