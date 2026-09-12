#pragma once

#include "balance.h"

typedef struct {
    uint16_t delay; // Simulation ticks after the preceding scripted launch
    uint8_t x, target, kind;
} McPuzzleLaunch;
uint8_t mc_puzzle_size(uint16_t puzzle);
const McPuzzleLaunch* mc_puzzle_launch(uint16_t puzzle, uint8_t index);
const char* mc_puzzle_name(uint16_t puzzle);
McWavePattern mc_wave_pattern(uint16_t wave);
const char* mc_wave_pattern_name(McWavePattern pattern);

uint32_t mc_rng_next(uint32_t* state);
uint16_t mc_rng_bounded(uint32_t* state, uint16_t bound);
void mc_wave_prepare(McGame* game, uint16_t enemy_total);
McEnemyKind mc_wave_take_next_kind(McGame* game);
McWaveModifier mc_wave_select_next_modifier(McGame* game, uint16_t next_wave);
uint8_t mc_wave_choose_target(McGame* game);
uint8_t mc_wave_choose_spawn_x(McGame* game);
uint16_t mc_wave_enemy_speed(const McWaveBalance* balance, McEnemyKind kind);
uint32_t mc_wave_enemy_points(McEnemyKind kind);
const char* mc_wave_modifier_name(McWaveModifier modifier);
const char* mc_wave_modifier_effect(McWaveModifier modifier, uint16_t wave);
