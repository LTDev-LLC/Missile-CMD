#pragma once
#include <stddef.h>
#include <stdint.h>
uint8_t mc_popcount32(uint32_t value);
void mc_write_u16(uint8_t* output, uint16_t value);
void mc_write_u32(uint8_t* output, uint32_t value);
uint16_t mc_read_u16(const uint8_t* input);
uint32_t mc_read_u32(const uint8_t* input);
uint32_t mc_crc32(const uint8_t* data, size_t size);

// -1 denotes an empty mask. __builtin_ctz is only evaluated for nonzero values.
static inline int8_t mc_mask_first(uint32_t mask) {
    return mask ? (int8_t)__builtin_ctz(mask) : -1;
}

// Remove the lowest occupied slot. Callers pass a nonzero mask.
static inline uint8_t mc_mask_take_first(uint32_t* mask) {
    const uint8_t index = (uint8_t)mc_mask_first(*mask);
    *mask &= *mask - 1U;
    return index;
}
