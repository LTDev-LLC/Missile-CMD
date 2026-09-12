#pragma once

#include "game_model.h"

uint32_t mc_distance_squared(int16_t ax, int16_t ay, int16_t bx, int16_t by);
uint16_t mc_integer_sqrt(uint32_t value);
// Speed is in hundredths of pixels per second; duration rounds up to game ticks
uint16_t mc_path_duration(
    int16_t start_x,
    int16_t start_y,
    int16_t target_x,
    int16_t target_y,
    uint16_t speed_x100);
void mc_path_init(
    McPath* path,
    int16_t start_x,
    int16_t start_y,
    int16_t target_x,
    int16_t target_y,
    uint16_t duration);
bool mc_path_step(McPath* path);
uint8_t mc_path_x(const McPath* path);
uint8_t mc_path_y(const McPath* path);
bool mc_point_in_radius(int16_t px, int16_t py, int16_t cx, int16_t cy, uint8_t radius);
