#include "binary.h"
#include "wave.h"
#include "text.h"
#include <stddef.h>

#include <limits.h>

static uint16_t mc_scale_u16(uint16_t value, uint16_t percent) {
    const uint32_t scaled = ((uint32_t)value * percent + 50U) / 100U;
    return scaled > UINT16_MAX ? UINT16_MAX : (uint16_t)scaled;
}

static uint16_t mc_capped_speed(uint16_t speed, uint16_t percent) {
    const uint16_t scaled = mc_scale_u16(speed, percent);
    return scaled > 2200U ? 2200U : scaled;
}

// Scale and cap baseline wave pressure before applying difficulty and modifier adjustments
McWaveBalance
    mc_balance_for_wave(uint16_t wave, McDifficulty difficulty, McWaveModifier modifier) {
    if(wave == 0U) wave = 1U;
    uint32_t enemy_total = 8U + (uint32_t)(wave - 1U) * 2U;
    if(enemy_total > MC_PENDING_CAPACITY) enemy_total = MC_PENDING_CAPACITY;
    uint32_t max_active = 2U + (uint32_t)(wave - 1U) / 6U;
    if(max_active > 6U) max_active = 6U;
    uint32_t speed = 550U + (uint32_t)(wave - 1U) * 40U;
    if(speed > 1400U) speed = 1400U;
    const uint32_t reduction = (uint32_t)(wave - 1U);
    uint32_t interval = reduction >= 18U ? 18U : 38U - reduction;

    uint16_t speed_percent = 100U;
    uint16_t pressure_percent = 100U;
    uint8_t radius = 9U;
    if(difficulty == McDifficultyCadet) {
        speed_percent = 80U;
        pressure_percent = 85U;
        radius = 11U;
    } else if(difficulty == McDifficultyCrisis) {
        speed_percent = 120U;
        pressure_percent = 125U;
        radius = 8U;
    }

    uint16_t base_speed = mc_scale_u16((uint16_t)speed, speed_percent);
    base_speed = mc_scale_u16(base_speed, 75U);
    // Higher pressure shortens spawn intervals through the reciprocal percentage
    uint16_t spawn_interval = mc_scale_u16((uint16_t)interval, 10000U / pressure_percent);
    const bool tier_two = wave >= 20U;
    if(modifier == McWaveModifierBarrage) {
        spawn_interval = mc_scale_u16(spawn_interval, tier_two ? 70U : 80U);
    } else if(modifier == McWaveModifierVelocity) {
        base_speed = mc_capped_speed(base_speed, tier_two ? 120U : 110U);
    } else if(modifier == McWaveModifierOverload) {
        max_active += tier_two ? 2U : 1U;
        if(max_active > 8U) max_active = 8U;
    }
    if(spawn_interval < 6U) spawn_interval = 6U;

    McWaveBalance result = {
        .spawn_interval_ticks = spawn_interval,
        .enemy_total = (uint16_t)enemy_total,
        .max_active_enemies = (uint8_t)max_active,
        .defensive_radius = radius,
    };
    result.kind_speed_x100[McEnemyStandard] = base_speed;
    result.kind_speed_x100[McEnemyFast] = mc_capped_speed(base_speed, 150U);
    result.kind_speed_x100[McEnemySplitter] = mc_capped_speed(base_speed, 90U);
    result.kind_speed_x100[McEnemySplitChild] = mc_capped_speed(base_speed, 110U);
    result.kind_speed_x100[McEnemyEvasive] = base_speed;
    return result;
}

uint32_t mc_rng_next(uint32_t* state) {
    uint32_t value = *state;
    // Xorshift cannot escape zero; substitute a nonzero state
    if(value == 0U) value = 0x6D2B79F5U;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

uint16_t mc_rng_bounded(uint32_t* state, uint16_t bound) {
    return bound == 0U ? 0U : (uint16_t)(mc_rng_next(state) % bound);
}

static uint8_t mc_nth_site(uint16_t mask, uint8_t ordinal) {
    mask &= MC_ALL_SITE_MASK;
    while(mask && ordinal) {
        mask &= mask - 1U;
        ordinal--;
    }
    const int8_t slot = mc_mask_first(mask);
    return slot < 0 ? 0U : (uint8_t)slot;
}

static uint8_t mc_choose_living_with_state(McGame* game, uint32_t* state) {
    const uint8_t count = mc_popcount32(game->alive_site_mask);
    return count == 0U ? 0U :
                         mc_nth_site(game->alive_site_mask, (uint8_t)mc_rng_bounded(state, count));
}

McWaveModifier mc_wave_select_next_modifier(McGame* game, uint16_t next_wave) {
    if(next_wave < 10U) return McWaveModifierNone;
    McWaveModifier candidate;
    do {
        candidate =
            (McWaveModifier)(1U +
                             mc_rng_bounded(&game->director_rng_state, McWaveModifierCount - 1U));
    } while(candidate == game->modifier);
    return candidate;
}

// Precompute kind counts and attack geometry; launches consume the roster without replacement
void mc_wave_prepare(McGame* game, uint16_t enemy_total) {
    if(enemy_total > MC_PENDING_CAPACITY) enemy_total = MC_PENDING_CAPACITY;
    game->wave_pattern = mc_wave_pattern(game->wave);

    uint8_t fast = 0U;
    uint8_t splitter = 0U;
    if(game->wave >= 3U) {
        const uint16_t count = 2U + (game->wave - 3U) / 3U;
        fast = (uint8_t)(count > 6U ? 6U : count);
    }
    if(game->wave >= 5U) {
        const uint16_t count = 1U + (game->wave - 5U) / 4U;
        splitter = (uint8_t)(count > 4U ? 4U : count);
    }
    if(fast > enemy_total) fast = (uint8_t)enemy_total;
    // Trim special kinds before subtracting them from the standard roster
    if((uint16_t)fast + splitter > enemy_total) splitter = (uint8_t)(enemy_total - fast);
    uint8_t standard = (uint8_t)(enemy_total - fast - splitter);

    if(game->modifier == McWaveModifierFastSwarm && standard > 0U) {
        const uint8_t divisor = game->wave >= 20U ? 2U : 3U;
        const uint8_t converted = (uint8_t)(standard / divisor);
        standard -= converted;
        fast += converted;
    } else if(game->modifier == McWaveModifierFracture && standard > 0U) {
        const uint8_t divisor = game->wave >= 20U ? 3U : 4U;
        const uint8_t converted = (uint8_t)(standard / divisor);
        standard -= converted;
        splitter += converted;
    }

    game->roster_remaining[0] = standard;
    game->roster_remaining[1] = fast;
    game->roster_remaining[2] = splitter;
    game->pending_remaining = (uint8_t)enemy_total;
    // Isolate roster randomness so target choices cannot reorder pending enemy kinds
    game->roster_rng_state = mc_rng_next(&game->rng_state);
    game->pattern_spawn_index = 0U;
    game->priority_site = game->modifier == McWaveModifierFocusFire ?
                              mc_choose_living_with_state(game, &game->director_rng_state) :
                              UINT8_MAX;
}

McEnemyKind mc_wave_take_next_kind(McGame* game) {
    if(game->pending_remaining == 0U) return McEnemyStandard;
    // Treat remaining kind counts as adjacent weighted ranges
    uint8_t pick = (uint8_t)mc_rng_bounded(&game->roster_rng_state, game->pending_remaining);
    McEnemyKind kind = McEnemyStandard;
    for(uint8_t bucket = 0U; bucket < 3U; bucket++) {
        if(pick < game->roster_remaining[bucket]) {
            kind = (McEnemyKind)bucket;
            game->roster_remaining[bucket]--;
            break;
        }
        pick -= game->roster_remaining[bucket];
    }
    game->pending_remaining--;
    return kind;
}

uint8_t mc_wave_choose_target(McGame* game) {
    if(game->mode == McModeTraining) return game->wave == 3U ? 7U : 4U;
    if(game->modifier == McWaveModifierFocusFire) {
        if(game->priority_site >= MC_SITE_COUNT ||
           (game->alive_site_mask & (1U << game->priority_site)) == 0U) {
            game->priority_site = mc_choose_living_with_state(game, &game->director_rng_state);
        }
        const uint8_t chance = game->wave >= 20U ? 67U : 50U;
        if(mc_rng_bounded(&game->rng_state, 100U) < chance) return game->priority_site;
    }
    const uint8_t living_count = mc_popcount32(game->alive_site_mask);
    // Favor living targets 75% of the time; ruined sites remain valid targets
    if(living_count > 0U && mc_rng_bounded(&game->rng_state, 4U) != 0U) {
        return mc_nth_site(
            game->alive_site_mask, (uint8_t)mc_rng_bounded(&game->rng_state, living_count));
    }
    return (uint8_t)mc_rng_bounded(&game->rng_state, MC_SITE_COUNT);
}

uint8_t mc_wave_choose_spawn_x(McGame* game) {
    if(game->mode == McModeTraining) {
        game->last_spawn_x = game->wave == 2U ? (game->pattern_spawn_index % 2U ? 65U : 60U) :
                                                (game->pattern_spawn_index % 2U ? 86U : 42U);
        game->pattern_spawn_index++;
        return game->last_spawn_x;
    }
    const uint8_t n = game->pattern_spawn_index;
    if(game->wave_pattern == McWavePatternSweep || game->wave_pattern == McWavePatternConverge) {
        uint8_t x;
        if(game->wave_pattern == McWavePatternSweep) {
            const uint8_t column = (n / 8U) & 1U ? 7U - n % 8U : n % 8U;
            x = 7U + column * 16U;
        } else {
            const uint8_t inset = ((n % 8U) / 2U) * 16U;
            x = n & 1U ? 119U - inset : 7U + inset;
        }
        game->pattern_spawn_index++;
        game->last_spawn_x = x;
        return x;
    }
    if(game->wave_pattern == McWavePatternFlanks || game->wave_pattern == McWavePatternGroups) {
        const bool left = ((game->wave_pattern == McWavePatternGroups ? n / 3U : n) & 1U) == 0U;
        game->pattern_spawn_index++;
        const uint8_t offset = (uint8_t)mc_rng_bounded(&game->rng_state, 30U);
        const uint8_t candidate = left ? (uint8_t)(2U + offset) : (uint8_t)(96U + offset);
        game->last_spawn_x = candidate;
        return candidate;
    }
    uint8_t candidate = 0U;
    // Try for seven-pixel spacing, but bound retries so spawning cannot stall
    for(uint8_t attempt = 0U; attempt < 6U; attempt++) {
        candidate = (uint8_t)(2U + mc_rng_bounded(&game->rng_state, MC_SCREEN_WIDTH - 4U));
        const int16_t separation = (int16_t)candidate - game->last_spawn_x;
        if(separation <= -7 || separation >= 7) break;
    }
    game->pattern_spawn_index++;
    game->last_spawn_x = candidate;
    return candidate;
}

uint16_t mc_wave_enemy_speed(const McWaveBalance* balance, McEnemyKind kind) {
    return kind < McEnemyKindCount ? balance->kind_speed_x100[kind] :
                                     balance->kind_speed_x100[McEnemyStandard];
}

uint32_t mc_wave_enemy_points(McEnemyKind kind) {
    return kind == McEnemyFast || kind == McEnemySplitter ? 50U : 25U;
}

const char* mc_wave_modifier_name(McWaveModifier modifier) {
    static const char Names[] = "Standard\0"
                                "Barrage\0"
                                "Velocity\0"
                                "Fast Swarm\0"
                                "Fracture\0"
                                "Focus Fire\0"
                                "Overload";
    return mc_text_at(Names, modifier < McWaveModifierCount ? modifier : 0);
}

const char* mc_wave_modifier_effect(McWaveModifier modifier, uint16_t wave) {
    const bool tier_two = wave >= 20U;
    switch(modifier) {
    case McWaveModifierBarrage:
        return tier_two ? "30% faster launches" : "20% faster launches";
    case McWaveModifierVelocity:
        return tier_two ? "20% missile speed" : "10% missile speed";
    case McWaveModifierFastSwarm:
        return tier_two ? "Half become fast" : "More fast missiles";
    case McWaveModifierFracture:
        return tier_two ? "Heavy splitters" : "More splitters";
    case McWaveModifierFocusFire:
        return tier_two ? "67% priority fire" : "50% priority fire";
    case McWaveModifierOverload:
        return tier_two ? "+2 active threats" : "+1 active threat";
    default:
        return "Standard attack";
    }
}

McWavePattern mc_wave_pattern(uint16_t wave) {
    if(wave <= 2U) return McWavePatternStaggered;
    static const uint8_t early[] = {
        McWavePatternSalvo, McWavePatternFlanks, McWavePatternStaggered};
    static const uint8_t late[] = {
        McWavePatternSweep,
        McWavePatternSalvo,
        McWavePatternConverge,
        McWavePatternFlanks,
        McWavePatternGroups,
        McWavePatternStaggered};
    return wave < 25U ? early[(wave - 3U) % 3U] : late[(wave - 25U) % 6U];
}

const char* mc_wave_pattern_name(McWavePattern pattern) {
    return mc_text_at(
        "Staggered\0Salvo\0Flanks\0Sweep\0Converge\0Alt groups",
        pattern < McWavePatternCount ? pattern : 0U);
}

static const McPuzzleLaunch McPuzzleLaunches[] = {
    {0, 46, 4, 0},  {1, 49, 4, 0},   {1, 52, 4, 0},  {0, 18, 1, 0},  {1, 23, 1, 0},
    {1, 102, 7, 0}, {1, 107, 7, 0},  {0, 12, 7, 0},  {1, 110, 1, 0}, {1, 16, 7, 0},
    {1, 106, 1, 0}, {1, 64, 4, 0},   {0, 18, 1, 0},  {1, 22, 1, 0},  {1, 26, 1, 0},
    {1, 100, 7, 0}, {1, 104, 7, 0},  {1, 108, 7, 0}, {0, 18, 1, 0},  {1, 22, 1, 0},
    {1, 26, 1, 0},  {80, 100, 7, 0}, {1, 104, 7, 0}, {1, 108, 7, 0}, {0, 42, 4, 0},
    {1, 48, 4, 0},  {1, 54, 4, 0},   {1, 60, 4, 0},  {1, 66, 4, 0},  {1, 72, 4, 0},
    {0, 12, 7, 0},  {1, 110, 1, 0},  {1, 16, 7, 1},  {1, 106, 1, 1}, {1, 64, 4, 0},
    {0, 44, 4, 2},  {1, 49, 4, 0},   {1, 54, 4, 2},  {0, 18, 1, 0},  {1, 22, 1, 1},
    {1, 26, 1, 0},  {80, 100, 7, 2}, {1, 104, 7, 1}, {1, 108, 7, 0},
};
// Adjacent offsets delimit a puzzle's launches; the final offset marks the end of the table
static const uint8_t McPuzzleOffsets[] = {0, 3, 7, 12, 18, 24, 30, 35, 38, 44};
uint8_t mc_puzzle_size(uint16_t puzzle) {
    return puzzle < MC_PUZZLE_COUNT ?
               (uint8_t)(McPuzzleOffsets[puzzle + 1U] - McPuzzleOffsets[puzzle]) :
               0U;
}
const McPuzzleLaunch* mc_puzzle_launch(uint16_t puzzle, uint8_t index) {
    if(puzzle >= MC_PUZZLE_COUNT || index >= mc_puzzle_size(puzzle)) return NULL;
    return &McPuzzleLaunches[McPuzzleOffsets[puzzle] + index];
}
const char* mc_puzzle_name(uint16_t puzzle) {
    return mc_text_at(
        "Cluster\0Twin fronts\0Crossing\0Double cluster\0Staggered fronts\0Chain bridge\0Fast crossing\0Split timing\0Mixed finale",
        puzzle < MC_PUZZLE_COUNT ? puzzle : 0U);
}
