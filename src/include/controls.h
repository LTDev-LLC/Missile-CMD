#pragma once

#include "persistence_codec.h"

#include <stdbool.h>
#include <stdint.h>

uint16_t mc_cursor_base_step_q8(McCursorSpeed speed);
uint16_t mc_cursor_step_q8(McCursorSpeed speed, uint16_t held_ticks, bool diagonal);
uint8_t mc_wrap_increment(uint8_t value, uint8_t count);
uint8_t mc_wrap_decrement(uint8_t value, uint8_t count);

uint8_t mc_wrap_step(uint8_t value, uint8_t count, bool increment);
uint8_t mc_list_first(uint8_t selected, uint8_t visible);
