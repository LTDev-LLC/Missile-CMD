#include "binary.h"
#include "game.h"
#include "text.h"

#include "balance.h"
#include "collision.h"
#include "wave.h"

#include <limits.h>
#include <string.h>

static const McSiteDef McSites[MC_SITE_COUNT] = {
    {.kind = McSiteBattery, .x = 6U, .y = 56U},
    {.kind = McSiteCity, .x = 17U, .y = 57U},
    {.kind = McSiteCity, .x = 31U, .y = 57U},
    {.kind = McSiteBattery, .x = 47U, .y = 56U},
    {.kind = McSiteCity, .x = 61U, .y = 57U},
    {.kind = McSiteCity, .x = 75U, .y = 57U},
    {.kind = McSiteBattery, .x = 91U, .y = 56U},
    {.kind = McSiteCity, .x = 105U, .y = 57U},
    {.kind = McSiteCity, .x = 120U, .y = 57U},
};

bool mc_options_valid(McGameMode mode, const McRunOptions* o) {
    return o && o->enemy <= 4U && o->slow <= 1U && o->unlimited <= 1U &&
           o->start_wave < UINT16_MAX &&
           (mc_mode_def(mode)->options == McOptionsPractice ||
            (o->enemy == 0U && !o->slow && !o->unlimited &&
             (o->start_wave == 0U || (mc_mode_def(mode)->options == McOptionsPuzzle &&
                                      o->start_wave < MC_PUZZLE_COUNT))));
}

bool mc_game_competitive(const McGame* game) {
    return mc_mode_has(game->mode, McModeCompetitive);
}
const char* mc_fire_reason(McFireResult result) {
    if(result == McFirePoolFull) return "8 shots in flight";
    if(result == McFireEmpty) return "Battery empty";
    if(result == McFireNoBattery) return "No batteries";
    return "Cannot fire now";
}

static const uint8_t McBatterySites[MC_BATTERY_COUNT] = {0U, 3U, 6U};

static int8_t mc_first_clear(uint32_t mask, uint8_t capacity) {
    const int8_t slot = mc_mask_first(~mask);
    return slot < capacity ? slot : -1;
}

static int8_t mc_battery_slot_for_site(uint8_t site_index) {
    for(uint8_t slot = 0U; slot < MC_BATTERY_COUNT; slot++) {
        if(McBatterySites[slot] == site_index) return (int8_t)slot;
    }
    return -1;
}

const McSiteDef* mc_game_site_def(uint8_t site_index) {
    return site_index < MC_SITE_COUNT ? &McSites[site_index] : NULL;
}

bool mc_game_site_alive(const McGame* game, uint8_t site_index) {
    return site_index < MC_SITE_COUNT && (game->alive_site_mask & (1U << site_index)) != 0U;
}

uint8_t mc_game_site_ammo(const McGame* game, uint8_t site_index) {
    const int8_t slot = mc_battery_slot_for_site(site_index);
    return slot >= 0 ? game->battery_ammo[(uint8_t)slot] : 0U;
}

uint8_t mc_game_cursor_x(const McGame* game) {
    return (uint8_t)(game->cursor_x_q8 >> MC_CURSOR_SHIFT);
}

uint8_t mc_game_cursor_y(const McGame* game) {
    return (uint8_t)(game->cursor_y_q8 >> MC_CURSOR_SHIFT);
}

void mc_game_events_clear(McGameEventBuffer* events) {
    if(!events) return;
    events->count = 0U;
    events->overflowed = false;
    events->explosion = false;
}

void mc_game_event_push(McGameEventBuffer* events, McGameEventType type, uint8_t chain_depth) {
    if(!events) return;
    if(events->count >= MC_GAME_EVENT_CAPACITY) {
        events->overflowed = true;
        return;
    }
    events->items[events->count++] = (McGameEvent){
        .type = type,
        .chain_depth = chain_depth,
    };
}

static void mc_score_add(McGame* game, uint32_t points) {
    game->score = UINT32_MAX - game->score < points ? UINT32_MAX : game->score + points;
    // A single bonus can cross several repair-credit thresholds
    while(game->score >= game->next_repair_score) {
        if(game->repair_credits < MC_REPAIR_CREDIT_CAP) game->repair_credits++;
        // Stop before threshold wrap could make this loop run forever
        if(UINT32_MAX - game->next_repair_score < MC_REPAIR_CREDIT_SCORE) {
            game->next_repair_score = UINT32_MAX;
            break;
        }
        game->next_repair_score += MC_REPAIR_CREDIT_SCORE;
    }
}

static uint8_t mc_effect_radius(uint8_t age, uint8_t maximum) {
    if(age < MC_EXPLOSION_EXPAND_TICKS) {
        const uint16_t radius = ((uint16_t)maximum * (age + 1U)) / MC_EXPLOSION_EXPAND_TICKS;
        return radius == 0U ? 1U : (uint8_t)radius;
    }
    if(age < MC_EXPLOSION_EXPAND_TICKS + MC_EXPLOSION_HOLD_TICKS) return maximum;
    if(age < MC_EXPLOSION_TOTAL_TICKS) {
        const uint16_t remaining = MC_EXPLOSION_TOTAL_TICKS - age;
        const uint16_t radius = ((uint16_t)maximum * remaining) / MC_EXPLOSION_EXPAND_TICKS;
        return radius == 0U ? 1U : (uint8_t)radius;
    }
    return 0U;
}

// Byte access preserves each typed pool's layout and aliasing rules.
static int8_t mc_effect_slot(uint32_t mask, uint8_t capacity, const void* pool, uint8_t stride) {
    int8_t free_slot = mc_first_clear(mask, capacity);
    if(free_slot >= 0) return free_slot;
    const unsigned char* bytes = pool;
    uint8_t oldest = 0;
    for(uint8_t i = 1; i < capacity; i++)
        if(bytes[i * stride + offsetof(McRootExplosion, age)] >
           bytes[oldest * stride + offsetof(McRootExplosion, age)])
            oldest = i;
    return capacity ? (int8_t)oldest : -1;
}
_Static_assert(
    offsetof(McRootExplosion, age) == offsetof(McChainExplosion, age) &&
        offsetof(McRootExplosion, age) == offsetof(McImpactEffect, age) &&
        offsetof(McRootExplosion, max_radius) == offsetof(McChainExplosion, max_radius) &&
        offsetof(McRootExplosion, max_radius) == offsetof(McImpactEffect, max_radius) &&
        offsetof(McRootExplosion, radius) == offsetof(McChainExplosion, radius) &&
        offsetof(McRootExplosion, radius) == offsetof(McImpactEffect, radius),
    "shared effect fields must have identical offsets");
// Return the occupied mask; expiry and derived radii share one traversal.
static uint32_t mc_effects_update(
    void* pool,
    uint8_t stride,
    uint32_t mask,
    uint8_t* count,
    bool advance,
    bool hold) {
    unsigned char* bytes = pool;
    uint32_t pending = mask;
    while(pending) {
        uint8_t i = mc_mask_take_first(&pending);
        unsigned char* effect = bytes + i * stride;
        uint8_t* age = effect + offsetof(McRootExplosion, age);
        if(advance && !(hold && *age >= 8U && *age <= 12U)) (*age)++;
        if(advance && *age >= MC_EXPLOSION_TOTAL_TICKS) {
            mask &= ~(1UL << i);
            (*count)--;
        } else
            effect[offsetof(McRootExplosion, radius)] =
                mc_effect_radius(*age, effect[offsetof(McRootExplosion, max_radius)]);
    }
    return mask;
}

// Full explosion pools replace their oldest effect so new blasts still take effect
static void mc_spawn_root(McGame* game, uint8_t x, uint8_t y) {
    int8_t slot = mc_effect_slot(
        game->root_mask, MC_ROOT_EXPLOSION_CAPACITY, game->roots, sizeof(game->roots[0]));
    if(slot < 0) return;
    const uint8_t index = (uint8_t)slot;
    if((game->root_mask & (1U << index)) == 0U) game->root_count++;
    game->root_mask |= (uint8_t)(1U << index);
    game->roots[index] = (McRootExplosion){
        .x = x,
        .y = y,
        .max_radius = game->balance.defensive_radius,
        .radius = mc_effect_radius(0U, game->balance.defensive_radius),
    };
}

static void mc_spawn_chain(McGame* game, uint8_t x, uint8_t y, uint8_t depth) {
    int8_t slot = mc_effect_slot(
        game->chain_mask, MC_CHAIN_EXPLOSION_CAPACITY, game->chains, sizeof(game->chains[0]));
    if(slot < 0) return;
    const uint8_t index = (uint8_t)slot;
    if((game->chain_mask & (1UL << index)) == 0U) game->chain_count++;
    game->chain_mask |= 1UL << index;
    game->chains[index] = (McChainExplosion){
        .x = x,
        .y = y,
        .max_radius = MC_CHAIN_RADIUS,
        .radius = mc_effect_radius(0U, MC_CHAIN_RADIUS),
        .chain_depth = depth,
    };
}

static void mc_spawn_impact(McGame* game, uint8_t x, uint8_t y) {
    int8_t slot = mc_effect_slot(
        game->impact_mask, MC_IMPACT_EFFECT_CAPACITY, game->impacts, sizeof(game->impacts[0]));
    if(slot < 0) return;
    const uint8_t index = (uint8_t)slot;
    if((game->impact_mask & (1U << index)) == 0U) game->impact_count++;
    game->impact_mask |= (uint8_t)(1U << index);
    game->impacts[index] = (McImpactEffect){
        .x = x,
        .y = y,
        .max_radius = MC_IMPACT_RADIUS,
        .radius = mc_effect_radius(0U, MC_IMPACT_RADIUS),
    };
}

uint8_t mc_game_alive_sites(const McGame* game) {
    return mc_popcount32(game->alive_site_mask);
}

uint8_t mc_game_alive_cities(const McGame* game) {
    uint8_t count = 0U;
    for(uint8_t i = 0U; i < MC_SITE_COUNT; i++) {
        if(McSites[i].kind == McSiteCity && mc_game_site_alive(game, i)) count++;
    }
    return count;
}

uint8_t mc_game_destroyed_sites(const McGame* game) {
    return (uint8_t)(MC_SITE_COUNT - mc_game_alive_sites(game));
}

uint8_t mc_game_total_ammo(const McGame* game) {
    uint8_t total = 0U;
    for(uint8_t slot = 0U; slot < MC_BATTERY_COUNT; slot++) {
        if(mc_game_site_alive(game, McBatterySites[slot])) total += game->battery_ammo[slot];
    }
    return total;
}

static int8_t mc_locked_site(McBatterySelection selection) {
    return selection == McBatteryLeft   ? 0 :
           selection == McBatteryCenter ? 3 :
           selection == McBatteryRight  ? 6 :
                                          -1;
}

static void mc_select_remaining_battery(McGame* game, uint8_t destroyed_site) {
    if(mc_locked_site(game->battery_selection) != (int8_t)destroyed_site) return;
    const int8_t destroyed_slot = mc_battery_slot_for_site(destroyed_site);
    if(destroyed_slot < 0) return;

    for(uint8_t pass = 0U; pass < 2U; pass++) {
        for(uint8_t offset = 1U; offset < MC_BATTERY_COUNT; offset++) {
            const uint8_t slot = (uint8_t)(((uint8_t)destroyed_slot + offset) % MC_BATTERY_COUNT);
            const uint8_t site_index = McBatterySites[slot];
            if(!mc_game_site_alive(game, site_index)) continue;
            if(pass == 0U && game->battery_ammo[slot] == 0U) continue;
            game->battery_selection = (McBatterySelection)(McBatteryLeft + slot);
            return;
        }
    }
    game->battery_selection = McBatteryAuto;
}

int8_t mc_game_selected_battery(const McGame* game) {
    const int8_t locked = mc_locked_site(game->battery_selection);
    // Manual selection stays locked even when empty; firing reports the refusal
    if(locked >= 0 && mc_game_site_alive(game, (uint8_t)locked)) return locked;
    int8_t selected = -1;
    int16_t best_distance = INT16_MAX;
    const uint8_t cursor_x = mc_game_cursor_x(game);
    for(uint8_t slot = 0U; slot < MC_BATTERY_COUNT; slot++) {
        const uint8_t site_index = McBatterySites[slot];
        if(!mc_game_site_alive(game, site_index) || game->battery_ammo[slot] == 0U) continue;
        int16_t distance = (int16_t)mc_game_site_def(site_index)->x - cursor_x;
        if(distance < 0) distance = (int16_t)-distance;
        // Auto ties favor the earlier battery
        if(distance < best_distance) {
            selected = (int8_t)site_index;
            best_distance = distance;
        }
    }
    return selected;
}

bool mc_game_has_motion(const McGame* game) {
    return game->enemy_count != 0U || game->interceptor_count != 0U || game->root_count != 0U ||
           game->chain_count != 0U || game->impact_count != 0U;
}

void mc_game_init(McGame* game) {
    memset(game, 0, sizeof(*game));
    game->alive_site_mask = MC_ALL_SITE_MASK;
    for(uint8_t i = 0U; i < MC_BATTERY_COUNT; i++)
        game->battery_ammo[i] = MC_AMMO_PER_BATTERY;
    game->phase = McGamePhaseIdle;
    game->difficulty = McDifficultyCommand;
    game->wave_pattern = McWavePatternStaggered;
    game->battery_selection = McBatteryAuto;
    game->priority_site = UINT8_MAX;
    game->last_lost_site = UINT8_MAX;
    game->cursor_x_q8 = (MC_SCREEN_WIDTH / 2U) << MC_CURSOR_SHIFT;
    game->cursor_y_q8 = 30U << MC_CURSOR_SHIFT;
    game->rng_state = 1U;
    game->rules_version = MC_RULES_VERSION;
    game->director_rng_state = 0xA341316CU;
    game->next_repair_score = MC_REPAIR_CREDIT_SCORE;
}

static void mc_clear_transients(McGame* game) {
    memset(game->enemies, 0, sizeof(game->enemies));
    memset(game->interceptors, 0, sizeof(game->interceptors));
    memset(game->roots, 0, sizeof(game->roots));
    memset(game->chains, 0, sizeof(game->chains));
    memset(game->impacts, 0, sizeof(game->impacts));
    game->enemy_mask = 0U;
    game->chain_mask = 0U;
    game->interceptor_mask = 0U;
    game->root_mask = 0U;
    game->impact_mask = 0U;
    game->enemy_count = 0U;
    game->interceptor_count = 0U;
    game->root_count = 0U;
    game->chain_count = 0U;
    game->impact_count = 0U;
}

// Build the normal wave, then apply mode-specific roster, balance, and resource overrides
static void mc_start_wave(McGame* game) {
    mc_clear_transients(game);
    memset(&game->wave_stats, 0, sizeof(game->wave_stats));
    if(mc_mode_def(game->mode)->completion == McCompleteCampaign)
        game->modifier = mc_mode_mission(game->wave);
    game->training_flags &= 24U;
    game->balance = mc_mode_balance(game);
    mc_wave_prepare(game, game->balance.enemy_total);
    if(game->mode == McModeTraining) {
        game->roster_remaining[0] = game->wave == 2U ? 4U : 2U;
        game->roster_remaining[1] = game->roster_remaining[2] = 0U;
        game->pending_remaining = game->roster_remaining[0];
        game->wave_pattern = game->wave == 2U ? McWavePatternSalvo : McWavePatternStaggered;
        // An empty left battery forces a selection change in the battery lesson
        if(game->wave == 3U) {
            game->battery_ammo[0] = 0U;
            game->battery_selection = McBatteryLeft;
        }
    }
    if(game->mode == McModePractice && game->options.enemy) {
        game->roster_remaining[0] = game->pending_remaining;
        game->roster_remaining[1] = game->roster_remaining[2] = 0U;
    }
    if(game->mode == McModePuzzle) {
        game->roster_remaining[0] = mc_puzzle_size(game->options.start_wave);
        game->roster_remaining[1] = game->roster_remaining[2] = 0U;
        game->pending_remaining = game->roster_remaining[0];
        game->wave_pattern = McWavePatternStaggered;
        for(uint8_t i = 0U; i < MC_BATTERY_COUNT; i++)
            game->battery_ammo[i] = i == 1U ? 2U : 0U;
    }
    game->spawn_cooldown = 0U;
    game->wave_start_alive_mask = game->alive_site_mask;
    game->last_wave_bonus = 0U;
    game->phase = McGamePhasePlaying;
}

void mc_game_start_run(McGame* game, McDifficulty difficulty, uint32_t seed) {
    mc_game_start_mode(game, difficulty, seed, McModeClassic);
}

void mc_game_start_mode(McGame* game, McDifficulty difficulty, uint32_t seed, McGameMode mode) {
    const McRunOptions options = {0};
    mc_game_start_config(game, difficulty, seed, mode, &options);
}

// Normalize setup and apply mode rules before seeding randomness and building the first wave
void mc_game_start_config(
    McGame* game,
    McDifficulty difficulty,
    uint32_t seed,
    McGameMode mode,
    const McRunOptions* options) {
    mc_game_init(game);
    game->mode = mode < McModeCount ? mode : McModeClassic;
    if(mc_options_valid(game->mode, options)) game->options = *options;
    game->difficulty = mc_mode_difficulty(game->mode, difficulty);
    // Puzzle randomness, including split-child targets, must ignore the entered seed
    if(game->mode == McModePuzzle) seed = 0x50555A5AU;
    game->rng_state = seed == 0U ? 1U : seed;
    game->seed = game->rng_state;
    // Keep modifier choices independent of the spawn RNG
    game->director_rng_state = game->rng_state ^ 0xA341316CU;
    if(game->director_rng_state == 0U) game->director_rng_state = 1U;
    const McModeDef* def = mc_mode_def(game->mode);
    game->wave =
        def->start_wave + (def->options == McOptionsPractice ? game->options.start_wave : 0U);
    game->alive_site_mask = def->initial_sites;
    for(uint8_t i = 0U; i < MC_BATTERY_COUNT; i++)
        game->battery_ammo[i] = def->ammo;
    mc_start_wave(game);
}

uint32_t mc_daily_seed(uint32_t timestamp) {
    uint32_t day = (timestamp / 86400U) ^ 0x4D434432U;
    return mc_rng_next(&day);
}

bool mc_game_ranked(const McGame* game) {
    return mc_mode_has(game->mode, McModeRanked);
}

bool mc_training_passed(const McGame* game) {
    // Lesson bits: hit, chain, alternate battery, repair, supply purchase
    const uint8_t required = game->wave == 1U ? 1U :
                             game->wave == 2U ? 2U :
                             game->wave == 3U ? 4U :
                                                24U;
    return (game->training_flags & required) == required;
}

void mc_game_begin_next_wave(McGame* game) {
    if(game->phase != McGamePhaseWaveResult) return;
    if(game->mode == McModeTraining && !mc_training_passed(game)) {
        // Failed lessons replay the same wave with intact sites
        game->alive_site_mask = MC_ALL_SITE_MASK;
    } else if(game->wave < UINT16_MAX)
        game->wave++;
    for(uint8_t slot = 0U; slot < MC_BATTERY_COUNT; slot++) {
        if(mc_game_site_alive(game, McBatterySites[slot]))
            game->battery_ammo[slot] = mc_mode_def(game->mode)->ammo + game->bonus_ammo;
    }
    game->bonus_ammo = 0U;
    game->supplies = game->next_supplies;
    game->next_supplies = 0U;
    game->radius_boost = game->next_radius_boost;
    game->next_radius_boost = 0U;
    game->modifier = game->next_modifier;
    mc_start_wave(game);
}

McGameChange mc_game_move_cursor_q8(McGame* game, int16_t dx_q8, int16_t dy_q8) {
    int32_t x = (int32_t)game->cursor_x_q8 + dx_q8;
    int32_t y = (int32_t)game->cursor_y_q8 + dy_q8;
    // Clamp in Q8 space to preserve fractional motion within the crosshair bounds
    const int32_t min_x = MC_CURSOR_MIN_X << MC_CURSOR_SHIFT;
    const int32_t max_x = MC_CURSOR_MAX_X << MC_CURSOR_SHIFT;
    const int32_t min_y = MC_CURSOR_MIN_Y << MC_CURSOR_SHIFT;
    const int32_t max_y = MC_CURSOR_MAX_Y << MC_CURSOR_SHIFT;
    if(x < min_x) x = min_x;
    if(x > max_x) x = max_x;
    if(y < min_y) y = min_y;
    if(y > max_y) y = max_y;
    if(x == game->cursor_x_q8 && y == game->cursor_y_q8) return McGameChangeNone;
    game->cursor_x_q8 = (uint16_t)x;
    game->cursor_y_q8 = (uint16_t)y;
    return McGameChangeCursor | McGameChangeHud;
}

bool mc_game_select_battery(McGame* game, McBatterySelection selection) {
    if(selection >= McBatterySelectionCount) return false;
    const int8_t site = mc_locked_site(selection);
    if(site >= 0 && !mc_game_site_alive(game, (uint8_t)site)) return false;
    game->battery_selection = selection;
    return true;
}

McFireResult mc_game_fire(McGame* game) {
    if(game->phase != McGamePhasePlaying) return McFireNotPlaying;
    const int8_t battery_index = mc_game_selected_battery(game);
    if(battery_index < 0)
        return mc_game_alive_sites(game) > mc_game_alive_cities(game) ? McFireEmpty :
                                                                        McFireNoBattery;
    const int8_t battery_slot = mc_battery_slot_for_site((uint8_t)battery_index);
    if(battery_slot < 0 || !mc_game_site_alive(game, (uint8_t)battery_index) ||
       game->battery_ammo[(uint8_t)battery_slot] == 0U) {
        return McFireEmpty;
    }
    const int8_t free_slot = mc_first_clear(game->interceptor_mask, MC_INTERCEPTOR_CAPACITY);
    if(free_slot < 0) return McFirePoolFull;
    const uint8_t slot = (uint8_t)free_slot;
    const McSiteDef* battery = mc_game_site_def((uint8_t)battery_index);
    const uint8_t target_x = mc_game_cursor_x(game);
    const uint8_t target_y = mc_game_cursor_y(game);
    const uint16_t duration = mc_game_aim_ticks(game);
    game->interceptors[slot] = (McInterceptor){
        .battery_site = (uint8_t)battery_index,
    };
    mc_path_init(
        &game->interceptors[slot].path, battery->x, battery->y, target_x, target_y, duration);
    game->interceptor_mask |= (uint8_t)(1U << slot);
    game->interceptor_count++;
    if(!(game->mode == McModePractice && game->options.unlimited))
        game->battery_ammo[(uint8_t)battery_slot]--;
    if(game->wave_stats.shots < UINT16_MAX) game->wave_stats.shots++;
    if(game->stats.shots_fired < UINT32_MAX) game->stats.shots_fired++;
    if(game->mode == McModeTraining && game->wave == 3U && battery_index != 0)
        game->training_flags |= 4U;
    return McFireSuccess;
}

static void mc_init_enemy(
    McEnemyMissile* enemy,
    McEnemyKind kind,
    uint8_t target_site,
    uint8_t start_x,
    uint8_t start_y,
    uint16_t speed_x100) {
    const McSiteDef* target = mc_game_site_def(target_site);
    const uint16_t duration = mc_path_duration(start_x, start_y, target->x, target->y, speed_x100);
    *enemy = (McEnemyMissile){
        .split_remaining = (uint16_t)(((uint32_t)duration * MC_SPLIT_REMAINING_PERCENT) / 100U),
        .origin_x = start_x,
        .origin_y = start_y,
        .target_site = target_site,
        .kind = kind,
    };
    mc_path_init(&enemy->path, start_x, start_y, target->x, target->y, duration);
}

static bool mc_spawn_enemy(McGame* game, McEnemyKind kind) {
    const int8_t free_slot = mc_first_clear(game->enemy_mask, MC_ENEMY_CAPACITY);
    if(free_slot < 0) return false;
    const uint8_t slot = (uint8_t)free_slot;
    uint8_t target, spawn_x;
    if(game->mode == McModePuzzle) {
        const McPuzzleLaunch* launch =
            mc_puzzle_launch(game->options.start_wave, game->pattern_spawn_index++);
        target = launch->target;
        spawn_x = launch->x;
        kind = launch->kind;
        game->last_spawn_x = spawn_x;
    } else {
        target = mc_wave_choose_target(game);
        spawn_x = mc_wave_choose_spawn_x(game);
        if(game->mode == McModePractice && game->options.enemy)
            kind = game->options.enemy == 4U ? McEnemyEvasive : game->options.enemy - 1U;
        if(game->mode == McModeEvasive && kind == McEnemyStandard) kind = McEnemyEvasive;
    }
    mc_init_enemy(
        &game->enemies[slot],
        kind,
        target,
        spawn_x,
        MC_PLAYFIELD_TOP,
        mc_wave_enemy_speed(&game->balance, kind));
    game->enemy_mask |= 1UL << slot;
    game->enemy_count++;
    return true;
}

static uint8_t mc_choose_living_target(McGame* game, uint8_t excluded) {
    uint8_t living[MC_SITE_COUNT];
    uint8_t count = 0U;
    for(uint8_t i = 0U; i < MC_SITE_COUNT; i++) {
        if(i != excluded && mc_game_site_alive(game, i)) living[count++] = i;
    }
    return count == 0U ? mc_wave_choose_target(game) :
                         living[mc_rng_bounded(&game->rng_state, count)];
}

static void mc_release_enemy(McGame* game, uint8_t slot) {
    if((game->enemy_mask & (1UL << slot)) == 0U) return;
    game->enemy_mask &= ~(1UL << slot);
    if(game->enemy_count > 0U) game->enemy_count--;
}

// Replace a splitter within the threat cap, preferring distinct living targets for its children
static void mc_split_enemy(McGame* game, uint8_t parent_slot) {
    // Copy before freeing: a child may immediately reuse the parent slot
    McEnemyMissile parent = game->enemies[parent_slot];
    const uint8_t x = mc_path_x(&parent.path);
    const uint8_t y = mc_path_y(&parent.path);
    mc_release_enemy(game, parent_slot);
    uint8_t spawned = 0U;
    uint8_t first_target = UINT8_MAX;
    for(uint8_t child = 0U; child < 2U && game->enemy_count < game->balance.max_active_enemies;
        child++) {
        const int8_t free_slot = mc_first_clear(game->enemy_mask, MC_ENEMY_CAPACITY);
        if(free_slot < 0) break;
        const uint8_t target = mc_choose_living_target(game, first_target);
        if(child == 0U) first_target = target;
        const uint8_t slot = (uint8_t)free_slot;
        mc_init_enemy(
            &game->enemies[slot],
            McEnemySplitChild,
            target,
            x,
            y,
            mc_wave_enemy_speed(&game->balance, McEnemySplitChild));
        game->enemy_mask |= 1UL << slot;
        game->enemy_count++;
        spawned++;
    }
    // If the threat cap prevents children, preserve the threat as a fast missile
    if(spawned == 0U) {
        mc_init_enemy(
            &game->enemies[parent_slot],
            McEnemyFast,
            parent.target_site,
            x,
            y,
            mc_wave_enemy_speed(&game->balance, McEnemyFast));
        game->enemy_mask |= 1UL << parent_slot;
        game->enemy_count++;
        return;
    }
}

// Return zero for a miss; hits also award accuracy credit to overlapping root blasts
static uint8_t mc_defensive_hit_depth(McGame* game, uint8_t x, uint8_t y) {
    uint8_t depth = 0U;
    uint32_t roots = game->root_mask;
    while(roots != 0U) {
        const uint8_t i = mc_mask_take_first(&roots);
        McRootExplosion* root = &game->roots[i];
        if(root->radius == 0U || !mc_point_in_radius(x, y, root->x, root->y, root->radius))
            continue;
        depth = depth < 1U ? 1U : depth;
        // Each shot earns accuracy credit once, even if its blast kills several missiles
        if((root->flags & MC_ROOT_HIT_CREDITED) == 0U) {
            root->flags |= MC_ROOT_HIT_CREDITED;
            if(game->stats.successful_shots < UINT32_MAX) game->stats.successful_shots++;
            if(game->wave_stats.hits < UINT16_MAX) game->wave_stats.hits++;
        }
    }
    uint32_t chains = game->chain_mask;
    while(chains != 0U) {
        const uint8_t i = mc_mask_take_first(&chains);
        const McChainExplosion* chain = &game->chains[i];
        // Overlapping blasts propagate the deepest chain
        if(chain->radius > 0U && chain->chain_depth > depth &&
           mc_point_in_radius(x, y, chain->x, chain->y, chain->radius)) {
            depth = chain->chain_depth;
        }
    }
    return depth;
}

static void mc_step_interceptors(McGame* game, McGameEventBuffer* events) {
    uint32_t mask = game->interceptor_mask;
    while(mask != 0U) {
        const uint8_t i = mc_mask_take_first(&mask);
        McInterceptor* interceptor = &game->interceptors[i];
        if(mc_path_step(&interceptor->path)) {
            game->interceptor_mask &= (uint8_t) ~(1U << i);
            if(game->interceptor_count > 0U) game->interceptor_count--;
            mc_spawn_root(game, interceptor->path.target_x, interceptor->path.target_y);
            if(events) events->explosion = true;
        }
    }
}

static void mc_step_enemies(McGame* game, McGameEventBuffer* events) {
    // Visit only threats present at tick start; splits may reuse or add slots
    uint32_t initial_mask = game->enemy_mask;
    while(initial_mask != 0U) {
        const uint8_t i = mc_mask_take_first(&initial_mask);
        if((game->enemy_mask & (1UL << i)) == 0U) continue;
        McEnemyMissile* enemy = &game->enemies[i];
        const bool arrived = mc_path_step(&enemy->path);
        const uint8_t x = mc_path_x(&enemy->path);
        const uint8_t y = mc_path_y(&enemy->path);
        const uint8_t hit_depth = mc_defensive_hit_depth(game, x, y);
        if(events && (hit_depth > 0U || arrived)) events->explosion = true;
        // Interception takes priority over splitting or ground impact at the same position
        if(hit_depth > 0U) {
            const McEnemyKind kind = enemy->kind;
            mc_release_enemy(game, i);
            const uint8_t multiplier = hit_depth > MC_CHAIN_CAP ? MC_CHAIN_CAP : hit_depth;
            if(game->mode == McModeTraining) game->training_flags |= multiplier > 1U ? 3U : 1U;
            const uint32_t points = mc_wave_enemy_points(kind) * multiplier;
            mc_score_add(game, points);
            if(game->stats.enemies_destroyed < UINT32_MAX) game->stats.enemies_destroyed++;
            if(game->wave_stats.kills < UINT16_MAX) game->wave_stats.kills++;
            if(multiplier > game->wave_stats.chain) game->wave_stats.chain = multiplier;
            if(multiplier > game->stats.max_chain_depth) game->stats.max_chain_depth = multiplier;
            mc_game_event_push(events, McGameEventEnemyDestroyed, multiplier);
            const uint8_t next_depth = hit_depth >= MC_CHAIN_CAP ? MC_CHAIN_CAP :
                                                                   (uint8_t)(hit_depth + 1U);
            mc_spawn_chain(game, x, y, next_depth);
            continue;
        }
        if(enemy->kind == McEnemySplitter && !arrived &&
           enemy->path.remaining <= enemy->split_remaining) {
            mc_split_enemy(game, i);
            continue;
        }
        if(enemy->kind == McEnemyEvasive && !arrived && enemy->split_remaining &&
           enemy->path.remaining <= enemy->split_remaining) {
            const uint8_t target = mc_choose_living_target(game, enemy->target_site);
            mc_init_enemy(
                enemy,
                McEnemyEvasive,
                target,
                x,
                y,
                mc_wave_enemy_speed(&game->balance, McEnemyEvasive));
            enemy->split_remaining = 0U;
            continue;
        }
        if(arrived) {
            if(game->mode == McModePuzzle) game->puzzle_failed = true;
            const uint8_t target_index = enemy->target_site;
            const McEnemyKind kind = enemy->kind;
            mc_release_enemy(game, i);
            if(mc_game_site_alive(game, target_index)) {
                game->last_lost_site = target_index;
                game->last_lost_enemy = kind;
                game->alive_site_mask &= (uint16_t) ~(1U << target_index);
                const int8_t battery_slot = mc_battery_slot_for_site(target_index);
                if(battery_slot >= 0) {
                    game->battery_ammo[(uint8_t)battery_slot] = 0U;
                    mc_select_remaining_battery(game, target_index);
                }
                if(game->stats.sites_lost < UINT16_MAX) game->stats.sites_lost++;
                mc_game_event_push(events, McGameEventSiteLost, 0U);
            }
            mc_spawn_impact(
                game, mc_game_site_def(target_index)->x, mc_game_site_def(target_index)->y);
        }
    }
}

static void mc_step_effects(McGame* game) {
    const bool hold = (game->supplies & 2U) && !(game->stats.play_ticks & 1U);
    game->root_mask = (uint8_t)mc_effects_update(
        game->roots, sizeof(game->roots[0]), game->root_mask, &game->root_count, true, hold);
    game->chain_mask = (uint32_t)mc_effects_update(
        game->chains, sizeof(game->chains[0]), game->chain_mask, &game->chain_count, true, hold);
    game->impact_mask = (uint8_t)mc_effects_update(
        game->impacts,
        sizeof(game->impacts[0]),
        game->impact_mask,
        &game->impact_count,
        true,
        false);
}

static void mc_finish_wave(McGame* game, McGameEventBuffer* events) {
    uint32_t bonus = (uint32_t)game->wave * 100U;
    bonus += (uint32_t)mc_game_alive_cities(game) * 100U;
    bonus += (uint32_t)mc_game_total_ammo(game) * 5U;
    // Perfect means no new losses this wave; earlier damage is allowed
    const bool perfect = game->alive_site_mask == game->wave_start_alive_mask;
    if(perfect) {
        bonus += 500U;
        if(game->stats.perfect_wave_streak < UINT8_MAX) game->stats.perfect_wave_streak++;
        if(game->stats.perfect_wave_streak > game->stats.best_perfect_wave_streak) {
            game->stats.best_perfect_wave_streak = game->stats.perfect_wave_streak;
        }
    } else {
        game->stats.perfect_wave_streak = 0U;
    }
    mc_score_add(game, bonus);
    // Only the HUD bonus is capped at 16 bits; the score received the full amount
    game->last_wave_bonus = bonus > UINT16_MAX ? UINT16_MAX : (uint16_t)bonus;
    if(game->wave <= MC_PACE_WAVES) game->wave_scores[game->wave - 1U] = game->score;
    game->phase = McGamePhaseWaveResult;
    game->next_modifier = mc_wave_select_next_modifier(
        game, (uint16_t)(game->wave < UINT16_MAX ? game->wave + 1U : UINT16_MAX));
    if(mc_mode_def(game->mode)->completion == McCompleteCampaign)
        game->next_modifier = mc_mode_mission(game->wave + 1U);
    // Seed damage and two credits for the repair-and-supply lesson
    if(game->mode == McModeTraining && game->wave == 3U) {
        game->alive_site_mask &= (uint16_t) ~(1U << 1U);
        game->repair_credits = 2U;
    }
    mc_game_event_push(events, McGameEventWaveCleared, 0U);
    if(((mc_mode_def(game->mode)->completion == McCompleteCampaign) &&
        game->wave >= MC_CAMPAIGN_WAVES) ||
       game->mode == McModePuzzle) {
        game->victory = game->mode != McModePuzzle || !game->puzzle_failed;
        game->phase = McGamePhaseGameOver;
        mc_game_event_push(events, McGameEventGameOver, 0U);
    }
}

// One deterministic tick: spawn, resolve combat, then choose defeat or wave-clear transitions
McGameChange mc_game_step(McGame* game, McGameEventBuffer* events) {
    mc_game_events_clear(events);
    if(game->phase != McGamePhasePlaying) return McGameChangeNone;
    if(game->stats.play_ticks < UINT32_MAX) game->stats.play_ticks++;
    if(mc_mode_def(game->mode)->completion == McCompleteSprint &&
       game->stats.play_ticks >= MC_SPRINT_TICKS) {
        game->victory = true;
        game->phase = McGamePhaseGameOver;
        mc_game_event_push(events, McGameEventGameOver, 0U);
        return McGameChangePhase | McGameChangeHud;
    }
    McGameChange changes = McGameChangeNone;
    if(game->spawn_cooldown > 0U) game->spawn_cooldown--;
    if(game->pending_remaining > 0U && game->spawn_cooldown == 0U &&
       game->enemy_count < game->balance.max_active_enemies) {
        const McEnemyKind kind = mc_wave_take_next_kind(game);
        if(mc_spawn_enemy(game, kind)) {
            if(game->mode == McModePuzzle && game->pending_remaining) {
                game->spawn_cooldown =
                    mc_puzzle_launch(game->options.start_wave, game->pattern_spawn_index)->delay;
            } else if(game->wave_pattern == McWavePatternSalvo) {
                game->spawn_cooldown = game->pattern_spawn_index % 3U == 0U ?
                                           (uint16_t)(game->balance.spawn_interval_ticks * 2U) :
                                           8U;
            } else {
                game->spawn_cooldown = game->balance.spawn_interval_ticks;
            }
            changes |= McGameChangeObjects;
        }
    }
    // Move interceptors first so new defensive blasts can intercept this tick
    if(game->interceptor_count > 0U) {
        mc_step_interceptors(game, events);
        changes |= McGameChangeObjects | McGameChangeEffects;
    }
    if(game->enemy_count > 0U) {
        mc_step_enemies(game, events);
        changes |= McGameChangeObjects | McGameChangeEffects;
    }
    // Age effects after collisions so every threat sees the same radius this tick
    if(game->root_count > 0U || game->chain_count > 0U || game->impact_count > 0U) {
        mc_step_effects(game);
        changes |= McGameChangeEffects;
    }
    for(uint8_t i = 0U; events && i < events->count; i++) {
        const McGameEventType type = events->items[i].type;
        if(type == McGameEventEnemyDestroyed) {
            changes |= McGameChangeHud;
        } else if(type == McGameEventSiteLost) {
            changes |= McGameChangeSites | McGameChangeHud;
        }
    }
    if(mc_game_alive_cities(game) == 0U ||
       (mc_mode_def(game->mode)->completion == McCompletePerfect &&
        game->alive_site_mask != game->wave_start_alive_mask)) {
        game->phase = McGamePhaseGameOver;
        mc_game_event_push(events, McGameEventGameOver, 0U);
        return changes | McGameChangePhase | McGameChangeSites | McGameChangeHud;
    }
    if(game->pending_remaining == 0U && game->enemy_count == 0U && game->interceptor_count == 0U &&
       game->root_count == 0U && game->chain_count == 0U && game->impact_count == 0U) {
        mc_finish_wave(game, events);
        changes |= McGameChangePhase | McGameChangeHud;
    }
    return changes;
}

McWorkshopResult mc_game_repair_reason(const McGame* game, uint8_t site_index) {
    if(game->phase != McGamePhaseWaveResult || site_index >= MC_SITE_COUNT ||
       !(mc_mode_def(game->mode)->repair_sites & (1U << site_index)))
        return McWorkshopUnavailable;
    if(mc_game_site_alive(game, site_index)) return McWorkshopIntact;
    return game->repair_credits ? McWorkshopOk : McWorkshopNoCredits;
}

const char* mc_workshop_reason(McWorkshopResult result) {
    return mc_text_at(
        "\0Unavailable\0No credits\0Site already intact\0Already bought\0No surviving battery\0Repair a site first",
        result <= McWorkshopRepairFirst ? result : McWorkshopUnavailable);
}

bool mc_game_repair_site(McGame* game, uint8_t site_index) {
    if(mc_game_repair_reason(game, site_index) != McWorkshopOk) return false;
    game->alive_site_mask |= (uint16_t)(1U << site_index);
    const int8_t battery_slot = mc_battery_slot_for_site(site_index);
    if(battery_slot >= 0) game->battery_ammo[(uint8_t)battery_slot] = MC_AMMO_PER_BATTERY;
    game->repair_credits--;
    if(game->stats.sites_repaired < UINT16_MAX) game->stats.sites_repaired++;
    if(game->mode == McModeTraining) game->training_flags |= 8U;
    return true;
}

McWorkshopResult mc_game_supply_reason(const McGame* game, McSupply supply) {
    if(game->phase != McGamePhaseWaveResult || supply >= McSupplyCount)
        return McWorkshopUnavailable;
    // The workshop lesson requires repair before a supply purchase
    if(game->mode == McModeTraining && game->wave == 3U && !(game->training_flags & 8U))
        return McWorkshopRepairFirst;
    if(supply == McSupplyAmmo && mc_game_alive_sites(game) == mc_game_alive_cities(game))
        return McWorkshopNoBattery;
    const bool bought =
        supply == McSupplyAmmo   ? game->bonus_ammo != 0U :
        supply == McSupplyRadius ? game->next_radius_boost != 0U :
                                   (game->next_supplies & (1U << (supply - McSupplySpeed))) != 0U;
    if(bought) return McWorkshopBought;
    return game->repair_credits ? McWorkshopOk : McWorkshopNoCredits;
}

bool mc_game_can_buy_supply(const McGame* game, McSupply supply) {
    return mc_game_supply_reason(game, supply) == McWorkshopOk;
}

bool mc_game_buy_supply(McGame* game, McSupply supply) {
    if(!mc_game_can_buy_supply(game, supply)) return false;
    if(supply == McSupplyAmmo) {
        game->bonus_ammo = 3U;
    } else if(supply == McSupplyRadius) {
        game->next_radius_boost = 2U;
    } else {
        game->next_supplies |= 1U << (supply - McSupplySpeed);
    }
    game->repair_credits--;
    if(game->mode == McModeTraining) game->training_flags |= 16U;
    return true;
}

// Restore derived counts, balance, selection, and radii omitted from saved state
void mc_game_rebuild_derived(McGame* game) {
    game->enemy_count = mc_popcount32(game->enemy_mask);
    game->interceptor_count = mc_popcount32(game->interceptor_mask);
    game->root_count = mc_popcount32(game->root_mask);
    game->chain_count = mc_popcount32(game->chain_mask);
    game->impact_count = mc_popcount32(game->impact_mask);
    game->balance = mc_mode_balance(game);
    const int8_t locked = mc_locked_site(game->battery_selection);
    if(locked >= 0 && !mc_game_site_alive(game, (uint8_t)locked))
        mc_select_remaining_battery(game, (uint8_t)locked);
    mc_effects_update(
        game->roots, sizeof(game->roots[0]), game->root_mask, &game->root_count, false, false);
    mc_effects_update(
        game->chains, sizeof(game->chains[0]), game->chain_mask, &game->chain_count, false, false);
    mc_effects_update(
        game->impacts,
        sizeof(game->impacts[0]),
        game->impact_mask,
        &game->impact_count,
        false,
        false);
}

static uint32_t mc_hash_byte(uint32_t hash, uint8_t value) {
    return (hash ^ value) * 16777619U;
}

static uint32_t mc_hash_u16(uint32_t hash, uint16_t value) {
    hash = mc_hash_byte(hash, (uint8_t)value);
    return mc_hash_byte(hash, (uint8_t)(value >> 8));
}

static uint32_t mc_hash_u32(uint32_t hash, uint32_t value) {
    hash = mc_hash_u16(hash, (uint16_t)value);
    return mc_hash_u16(hash, (uint16_t)(value >> 16));
}

// Hash fields individually so struct padding cannot affect the result
static uint32_t mc_hash_path(uint32_t hash, const McPath* path) {
    hash = mc_hash_u16(hash, path->x_q9);
    hash = mc_hash_u16(hash, path->y_q9);
    hash = mc_hash_u16(hash, (uint16_t)path->vx_q9);
    hash = mc_hash_u16(hash, (uint16_t)path->vy_q9);
    hash = mc_hash_u16(hash, path->remaining);
    hash = mc_hash_byte(hash, path->target_x);
    return mc_hash_byte(hash, path->target_y);
}

uint32_t mc_game_state_hash(const McGame* game) {
    uint32_t hash = 2166136261U;
    hash = mc_hash_u32(hash, game->enemy_mask);
    hash = mc_hash_u32(hash, game->chain_mask);
    hash = mc_hash_byte(hash, game->interceptor_mask);
    hash = mc_hash_byte(hash, game->root_mask);
    hash = mc_hash_byte(hash, game->impact_mask);
    // Ignore unused slots; their stale contents are not part of game state
    for(uint8_t i = 0U; i < MC_ENEMY_CAPACITY; i++) {
        if((game->enemy_mask & (1UL << i)) == 0U) continue;
        hash = mc_hash_path(hash, &game->enemies[i].path);
        hash = mc_hash_u16(hash, game->enemies[i].split_remaining);
        hash = mc_hash_byte(hash, game->enemies[i].target_site);
        hash = mc_hash_byte(hash, game->enemies[i].kind);
        hash = mc_hash_byte(hash, game->enemies[i].origin_x);
        hash = mc_hash_byte(hash, game->enemies[i].origin_y);
    }
    for(uint8_t i = 0U; i < MC_INTERCEPTOR_CAPACITY; i++) {
        if((game->interceptor_mask & (1U << i)) == 0U) continue;
        hash = mc_hash_path(hash, &game->interceptors[i].path);
        hash = mc_hash_byte(hash, game->interceptors[i].battery_site);
    }
    for(uint8_t i = 0U; i < MC_ROOT_EXPLOSION_CAPACITY; i++) {
        if((game->root_mask & (1U << i)) == 0U) continue;
        const McRootExplosion* effect = &game->roots[i];
        hash = mc_hash_byte(hash, effect->x);
        hash = mc_hash_byte(hash, effect->y);
        hash = mc_hash_byte(hash, effect->age);
        hash = mc_hash_byte(hash, effect->max_radius);
        hash = mc_hash_byte(hash, effect->flags);
    }
    for(uint8_t i = 0U; i < MC_CHAIN_EXPLOSION_CAPACITY; i++) {
        if((game->chain_mask & (1UL << i)) == 0U) continue;
        const McChainExplosion* effect = &game->chains[i];
        hash = mc_hash_byte(hash, effect->x);
        hash = mc_hash_byte(hash, effect->y);
        hash = mc_hash_byte(hash, effect->age);
        hash = mc_hash_byte(hash, effect->max_radius);
        hash = mc_hash_byte(hash, effect->chain_depth);
    }
    for(uint8_t i = 0U; i < MC_IMPACT_EFFECT_CAPACITY; i++) {
        if((game->impact_mask & (1U << i)) == 0U) continue;
        const McImpactEffect* effect = &game->impacts[i];
        hash = mc_hash_byte(hash, effect->x);
        hash = mc_hash_byte(hash, effect->y);
        hash = mc_hash_byte(hash, effect->age);
        hash = mc_hash_byte(hash, effect->max_radius);
    }
    hash = mc_hash_u32(hash, game->stats.shots_fired);
    hash = mc_hash_u32(hash, game->stats.successful_shots);
    hash = mc_hash_u32(hash, game->stats.enemies_destroyed);
    hash = mc_hash_u32(hash, game->stats.play_ticks);
    hash = mc_hash_u16(hash, game->stats.sites_lost);
    hash = mc_hash_u16(hash, game->stats.sites_repaired);
    hash = mc_hash_byte(hash, game->stats.max_chain_depth);
    hash = mc_hash_byte(hash, game->stats.perfect_wave_streak);
    hash = mc_hash_byte(hash, game->stats.best_perfect_wave_streak);
    hash = mc_hash_u32(hash, game->score);
    hash = mc_hash_u32(hash, game->rng_state);
    hash = mc_hash_u32(hash, game->roster_rng_state);
    hash = mc_hash_u32(hash, game->director_rng_state);
    hash = mc_hash_u32(hash, game->next_repair_score);
    hash = mc_hash_u16(hash, game->alive_site_mask);
    hash = mc_hash_u16(hash, game->wave_start_alive_mask);
    hash = mc_hash_u16(hash, game->cursor_x_q8);
    hash = mc_hash_u16(hash, game->cursor_y_q8);
    hash = mc_hash_u16(hash, game->wave);
    hash = mc_hash_u16(hash, game->spawn_cooldown);
    hash = mc_hash_u16(hash, game->last_wave_bonus);
    for(uint8_t i = 0U; i < MC_BATTERY_COUNT; i++)
        hash = mc_hash_byte(hash, game->battery_ammo[i]);
    for(uint8_t i = 0U; i < 3U; i++)
        hash = mc_hash_byte(hash, game->roster_remaining[i]);
    hash = mc_hash_byte(hash, game->pending_remaining);
    hash = mc_hash_byte(hash, game->phase);
    hash = mc_hash_byte(hash, game->difficulty);
    hash = mc_hash_byte(hash, game->wave_pattern);
    hash = mc_hash_byte(hash, game->modifier);
    hash = mc_hash_byte(hash, game->next_modifier);
    hash = mc_hash_byte(hash, game->battery_selection);
    hash = mc_hash_byte(hash, game->priority_site);
    hash = mc_hash_byte(hash, game->pattern_spawn_index);
    hash = mc_hash_byte(hash, game->repair_credits);
    hash = mc_hash_u32(hash, game->seed);
    hash = mc_hash_byte(hash, game->mode);
    hash = mc_hash_byte(hash, game->bonus_ammo);
    hash = mc_hash_byte(hash, game->radius_boost);
    hash = mc_hash_byte(hash, game->next_radius_boost);
    hash = mc_hash_byte(hash, game->training_flags);
    hash = mc_hash_byte(hash, game->last_spawn_x);
    hash = mc_hash_byte(hash, game->rules_version);
    hash = mc_hash_u16(hash, game->options.start_wave);
    hash = mc_hash_byte(hash, game->options.enemy);
    hash = mc_hash_byte(hash, game->options.slow);
    hash = mc_hash_byte(hash, game->options.unlimited);
    hash = mc_hash_byte(hash, game->last_lost_site);
    hash = mc_hash_byte(hash, game->last_lost_enemy);
    hash = mc_hash_byte(hash, game->puzzle_failed);
    hash = mc_hash_byte(hash, game->supplies);
    hash = mc_hash_byte(hash, game->next_supplies);
    hash = mc_hash_byte(hash, game->victory);
    hash = mc_hash_u16(hash, game->wave_stats.shots);
    hash = mc_hash_u16(hash, game->wave_stats.hits);
    hash = mc_hash_u16(hash, game->wave_stats.kills);
    hash = mc_hash_byte(hash, game->wave_stats.chain);
    for(uint8_t i = 0U; i < MC_PACE_WAVES; i++)
        hash = mc_hash_u32(hash, game->wave_scores[i]);

    return hash;
}

// Check ranges, pool consistency, and trajectories before saving or accepting restored state
bool mc_game_validate(const McGame* game) {
    if(!game || !mc_options_valid(game->mode, &game->options) ||
       game->rules_version != MC_RULES_VERSION || game->supplies > 3U ||
       game->next_supplies > 3U || game->wave_stats.hits > game->wave_stats.shots ||
       game->mode >= McModeCount || game->training_flags > 31U || game->bonus_ammo > 3U ||
       game->radius_boost > 2U || game->next_radius_boost > 2U ||
       game->difficulty >= McDifficultyCount || game->phase > McGamePhaseGameOver ||
       game->wave_pattern >= McWavePatternCount || game->modifier >= McWaveModifierCount ||
       game->next_modifier >= McWaveModifierCount ||
       game->battery_selection >= McBatterySelectionCount || game->wave == 0U ||
       (game->priority_site != UINT8_MAX && game->priority_site >= MC_SITE_COUNT)) {
        return false;
    }
    if((game->last_lost_site != UINT8_MAX && game->last_lost_site >= MC_SITE_COUNT) ||
       game->last_lost_enemy >= McEnemyKindCount ||
       (game->mode != McModePuzzle && game->puzzle_failed))
        return false;
    if(game->mode == McModePuzzle &&
       (game->pattern_spawn_index > mc_puzzle_size(game->options.start_wave) ||
        game->pattern_spawn_index + game->pending_remaining !=
            mc_puzzle_size(game->options.start_wave)))
        return false;
    if(game->alive_site_mask & (uint16_t)~MC_ALL_SITE_MASK) return false;
    if(mc_game_cursor_x(game) < MC_CURSOR_MIN_X || mc_game_cursor_x(game) > MC_CURSOR_MAX_X ||
       mc_game_cursor_y(game) < MC_CURSOR_MIN_Y || mc_game_cursor_y(game) > MC_CURSOR_MAX_Y) {
        return false;
    }
    if(game->enemy_mask & ~((1UL << MC_ENEMY_CAPACITY) - 1UL) ||
       game->chain_mask & ~((1UL << MC_CHAIN_EXPLOSION_CAPACITY) - 1UL) ||
       game->interceptor_mask & (uint8_t) ~((1U << MC_INTERCEPTOR_CAPACITY) - 1U) ||
       game->root_mask & (uint8_t) ~((1U << MC_ROOT_EXPLOSION_CAPACITY) - 1U) ||
       game->impact_mask & (uint8_t) ~((1U << MC_IMPACT_EFFECT_CAPACITY) - 1U)) {
        return false;
    }
    if(game->enemy_count != mc_popcount32(game->enemy_mask) ||
       game->interceptor_count != mc_popcount32(game->interceptor_mask) ||
       game->root_count != mc_popcount32(game->root_mask) ||
       game->chain_count != mc_popcount32(game->chain_mask) ||
       game->impact_count != mc_popcount32(game->impact_mask) || game->enemy_count > 8U ||
       game->balance.max_active_enemies > 8U || game->pending_remaining > MC_PENDING_CAPACITY ||
       game->repair_credits > MC_REPAIR_CREDIT_CAP ||
       (uint16_t)game->roster_remaining[0] + game->roster_remaining[1] +
               game->roster_remaining[2] !=
           game->pending_remaining ||
       game->stats.successful_shots > game->stats.shots_fired ||
       game->stats.perfect_wave_streak > game->stats.best_perfect_wave_streak) {
        return false;
    }
    for(uint8_t i = 0U; i < MC_BATTERY_COUNT; i++) {
        if(game->battery_ammo[i] > MC_AMMO_PER_BATTERY + 3U) return false;
        if(!mc_game_site_alive(game, McBatterySites[i]) && game->battery_ammo[i] != 0U)
            return false;
    }
    for(uint8_t i = 0U; i < MC_ENEMY_CAPACITY; i++) {
        if((game->enemy_mask & (1UL << i)) == 0U) continue;
        const McEnemyMissile* enemy = &game->enemies[i];
        const McSiteDef* target = mc_game_site_def(enemy->target_site);
        if(enemy->kind >= McEnemyKindCount || enemy->target_site >= MC_SITE_COUNT ||
           enemy->path.remaining == 0U ||
           enemy->path.y_q9 >= (MC_SCREEN_HEIGHT << MC_PATH_SHIFT) || !target ||
           enemy->path.target_x != target->x || enemy->path.target_y != target->y) {
            return false;
        }
    }
    for(uint8_t i = 0U; i < MC_INTERCEPTOR_CAPACITY; i++) {
        if((game->interceptor_mask & (1U << i)) != 0U &&
           (game->interceptors[i].battery_site >= MC_SITE_COUNT ||
            mc_battery_slot_for_site(game->interceptors[i].battery_site) < 0 ||
            game->interceptors[i].path.remaining == 0U ||
            game->interceptors[i].path.y_q9 >= (MC_SCREEN_HEIGHT << MC_PATH_SHIFT) ||
            game->interceptors[i].path.target_x < MC_CURSOR_MIN_X ||
            game->interceptors[i].path.target_x > MC_CURSOR_MAX_X ||
            game->interceptors[i].path.target_y < MC_CURSOR_MIN_Y ||
            game->interceptors[i].path.target_y > MC_CURSOR_MAX_Y))
            return false;
    }
    for(uint8_t i = 0U; i < MC_ROOT_EXPLOSION_CAPACITY; i++) {
        if((game->root_mask & (1U << i)) != 0U &&
           (game->roots[i].age >= MC_EXPLOSION_TOTAL_TICKS ||
            game->roots[i].flags & (uint8_t)~MC_ROOT_HIT_CREDITED))
            return false;
    }
    for(uint8_t i = 0U; i < MC_CHAIN_EXPLOSION_CAPACITY; i++) {
        if((game->chain_mask & (1UL << i)) != 0U &&
           (game->chains[i].age >= MC_EXPLOSION_TOTAL_TICKS || game->chains[i].chain_depth == 0U ||
            game->chains[i].chain_depth > MC_CHAIN_CAP))
            return false;
    }
    for(uint8_t i = 0U; i < MC_IMPACT_EFFECT_CAPACITY; i++) {
        if((game->impact_mask & (1U << i)) != 0U &&
           game->impacts[i].age >= MC_EXPLOSION_TOTAL_TICKS)
            return false;
    }
    const uint16_t roster_total = (uint16_t)game->roster_remaining[0] + game->roster_remaining[1] +
                                  game->roster_remaining[2];
    if(roster_total != game->pending_remaining) return false;
    return true;
}

uint16_t mc_game_aim_ticks(const McGame* game) {
    if(game->phase != McGamePhasePlaying || game->interceptor_count >= MC_INTERCEPTOR_CAPACITY)
        return 0U;
    const int8_t site = mc_game_selected_battery(game);
    if(site < 0 || !mc_game_site_ammo(game, (uint8_t)site)) return 0U;
    const McSiteDef* battery = mc_game_site_def((uint8_t)site);
    return mc_path_duration(
        battery->x,
        battery->y,
        mc_game_cursor_x(game),
        mc_game_cursor_y(game),
        MC_INTERCEPTOR_SPEED_X100 * ((game->supplies & 1U) ? 3U : 2U) / 2U);
}

uint16_t mc_game_impact_warnings(const McGame* game) {
    uint16_t sites = 0U;
    uint32_t mask = game->enemy_mask;
    while(mask) {
        const McEnemyMissile* enemy = &game->enemies[mc_mask_take_first(&mask)];
        if(enemy->path.remaining <= MC_TICKS_PER_SECOND)
            sites |= (uint16_t)(1U << enemy->target_site);
    }
    return sites & game->alive_site_mask;
}

uint8_t mc_game_debrief_id(const McGame* game) {
    uint8_t tip = 6U;
    if(game->victory)
        tip = 0U;
    else if(mc_game_alive_sites(game) == mc_game_alive_cities(game))
        tip = 1U;
    else if(!mc_game_total_ammo(game))
        tip = 2U;
    else if(
        game->stats.shots_fired >= 5U &&
        (uint64_t)game->stats.successful_shots * 2U < game->stats.shots_fired)
        tip = 3U;
    else if(
        game->last_lost_site != UINT8_MAX &&
        (game->last_lost_enemy == McEnemySplitter || game->last_lost_enemy == McEnemySplitChild))
        tip = 4U;
    else if(game->stats.enemies_destroyed >= 6U && game->stats.max_chain_depth < 2U)
        tip = 5U;
    return tip;
}
