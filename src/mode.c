#include "text.h"
#include "mode.h"
#include "balance.h"
#include "game.h"

#define NORMAL (McModeCompetitive | McModeSave | McModeReplay)
#define ALL    MC_ALL_SITE_MASK
#define ONE    ((1U << 0U) | (1U << 3U) | (1U << 4U) | (1U << 6U))
#define MODE(sites, wave, difficulty, seed, options, end, ammo, flags) \
    {sites, sites, wave, difficulty, seed, options, end, ammo, flags}
static const char McModeNames[] = "Classic\0"
                                  "Daily\0"
                                  "Seeded\0"
                                  "Limited ammo\0"
                                  "One city\0"
                                  "Barrage\0"
                                  "Perfect defense\0"
                                  "Endless Crisis\0"
                                  "Training\0"
                                  "Practice\0"
                                  "Campaign\0"
                                  "Score attack\0"
                                  "Puzzles\0"
                                  "Two players\0"
                                  "Evasive";

// Modes keep their public IDs while identical policies share immutable records.
static const uint8_t McModeRules[McModeCount] = {0, 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 4};
static const McModeDef McModes[] = {
    MODE(
        ALL,
        1,
        McDifficultyCount,
        McSeedRandom,
        McOptionsNone,
        McCompleteEndless,
        10,
        NORMAL | McModeRanked),
    MODE(ALL, 1, McDifficultyCommand, McSeedDaily, McOptionsNone, McCompleteEndless, 10, NORMAL),
    MODE(ALL, 1, McDifficultyCount, McSeedRandom, McOptionsNone, McCompleteEndless, 5, NORMAL),
    MODE(ONE, 1, McDifficultyCount, McSeedRandom, McOptionsNone, McCompleteEndless, 10, NORMAL),
    MODE(ALL, 1, McDifficultyCount, McSeedRandom, McOptionsNone, McCompleteEndless, 10, NORMAL),
    MODE(ALL, 1, McDifficultyCount, McSeedRandom, McOptionsNone, McCompletePerfect, 10, NORMAL),
    MODE(ALL, 10, McDifficultyCrisis, McSeedRandom, McOptionsNone, McCompleteEndless, 10, NORMAL),
    MODE(
        ALL,
        1,
        McDifficultyCadet,
        McSeedTraining,
        McOptionsNone,
        McCompleteLessons,
        10,
        McModeSave),
    MODE(
        ALL,
        1,
        McDifficultyCount,
        McSeedRandom,
        McOptionsPractice,
        McCompleteEndless,
        10,
        McModeReplay),
    MODE(ALL, 1, McDifficultyCount, McSeedRandom, McOptionsNone, McCompleteCampaign, 10, NORMAL),
    MODE(ALL, 1, McDifficultyCount, McSeedRandom, McOptionsNone, McCompleteSprint, 10, NORMAL),
    MODE(
        ALL,
        1,
        McDifficultyCount,
        McSeedPuzzle,
        McOptionsPuzzle,
        McCompletePuzzle,
        10,
        McModeCompetitive | McModeSave),
    MODE(ALL, 1, McDifficultyCount, McSeedRandom, McOptionsNone, McCompleteCampaign, 10, 0),
};
#undef MODE
#undef ALL
#undef ONE
#undef NORMAL

const McModeDef* mc_mode_def(McGameMode mode) {
    return &McModes[McModeRules[mode < McModeCount ? mode : McModeClassic]];
}
bool mc_mode_has(McGameMode mode, uint8_t flag) {
    return mode < McModeCount && (mc_mode_def(mode)->flags & flag) != 0;
}
McDifficulty mc_mode_difficulty(McGameMode mode, McDifficulty selected) {
    const uint8_t fixed = mc_mode_def(mode)->difficulty;
    return fixed < McDifficultyCount    ? fixed :
           selected < McDifficultyCount ? selected :
                                          McDifficultyCommand;
}
McWaveModifier mc_mode_mission(uint16_t wave) {
    static const uint8_t missions[MC_CAMPAIGN_WAVES] = {
        McWaveModifierNone,
        McWaveModifierBarrage,
        McWaveModifierVelocity,
        McWaveModifierFracture,
        McWaveModifierFocusFire};
    return missions[(wave - 1U) % MC_CAMPAIGN_WAVES];
}
McWaveBalance mc_mode_balance(const McGame* game) {
    McWaveBalance b = mc_balance_for_wave(game->wave, game->difficulty, game->modifier);
    b.defensive_radius += game->radius_boost;
    if(game->mode == McModeBarrage) b.spawn_interval_ticks = 8U;
    if(game->mode == McModeTraining) {
        if(game->wave == 2U) b.defensive_radius = 4U + game->radius_boost;
        b.max_active_enemies = 2U;
        b.spawn_interval_ticks = 55U;
        for(uint8_t i = 0; i < McEnemyKindCount; i++)
            b.kind_speed_x100[i] = 300U;
    }
    if(game->mode == McModePuzzle) {
        b.max_active_enemies = 6U;
        b.spawn_interval_ticks = 1U;
    }
    return b;
}

const char* mc_mode_name(McGameMode mode) {
    return mc_text_at(McModeNames, mode < McModeCount ? mode : McModeClassic);
}

uint32_t mc_mode_seed(McGameMode mode, uint32_t seed, uint32_t timestamp, bool reuse) {
    switch(mc_mode_def(mode)->seed_policy) {
    case McSeedTraining:
        return 0x54524149U;
    case McSeedPuzzle:
        return 0x50555A5AU;
    case McSeedDaily:
        if(!reuse) return mc_daily_seed(timestamp);
        break;
    default:
        break;
    }
    return seed ? seed : 1U;
}

bool mc_session_has(McGameMode mode, bool wave_practice, uint8_t flag) {
    return (!wave_practice || !(flag & (McModeSave | McModeCompetitive | McModeRanked))) &&
           mc_mode_has(mode, flag);
}
