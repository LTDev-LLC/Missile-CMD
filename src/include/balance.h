#pragma once

#include "game_model.h"

#define MC_TICKS_PER_SECOND       30U
#define MC_EXPLOSION_EXPAND_TICKS 8U
#define MC_EXPLOSION_HOLD_TICKS   5U
#define MC_EXPLOSION_TOTAL_TICKS \
    (MC_EXPLOSION_EXPAND_TICKS + MC_EXPLOSION_HOLD_TICKS + MC_EXPLOSION_EXPAND_TICKS)
#define MC_IMPACT_RADIUS           5U
#define MC_CHAIN_RADIUS            6U
#define MC_CHAIN_CAP               5U
#define MC_INTERCEPTOR_SPEED_X100  4200U
#define MC_SPLIT_REMAINING_PERCENT 45U

McWaveBalance mc_balance_for_wave(uint16_t wave, McDifficulty difficulty, McWaveModifier modifier);
