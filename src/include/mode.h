#pragma once

#include "game_model.h"

enum {
    McSeedRandom,
    McSeedDaily,
    McSeedTraining,
    McSeedPuzzle
};
enum {
    McOptionsNone,
    McOptionsPractice,
    McOptionsPuzzle
};
enum {
    McCompleteEndless,
    McCompleteLessons,
    McCompleteCampaign,
    McCompleteSprint,
    McCompletePuzzle,
    McCompletePerfect
};
enum {
    McModeRanked = 1U << 0,
    McModeCompetitive = 1U << 1,
    McModeSave = 1U << 2,
    McModeReplay = 1U << 3,
};

typedef struct {
    uint16_t initial_sites, repair_sites, start_wave;
    uint8_t difficulty; // McDifficultyCount means player-selected
    uint8_t seed_policy, options, completion, ammo, flags;
} McModeDef;

const McModeDef* mc_mode_def(McGameMode mode);
bool mc_mode_has(McGameMode mode, uint8_t flag);
McDifficulty mc_mode_difficulty(McGameMode mode, McDifficulty selected);
McWaveModifier mc_mode_mission(uint16_t wave);
McWaveBalance mc_mode_balance(const McGame* game);

uint32_t mc_mode_seed(McGameMode mode, uint32_t seed, uint32_t timestamp, bool reuse);

bool mc_session_has(McGameMode mode, bool wave_practice, uint8_t flag);
