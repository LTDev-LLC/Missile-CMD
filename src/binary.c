#include "binary.h"

uint8_t mc_popcount32(uint32_t value) {
    uint8_t count = 0U;
    while(value != 0U) {
        value &= value - 1U;
        count++;
    }
    return count;
}

void mc_write_u16(uint8_t* output, uint16_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

void mc_write_u32(uint8_t* output, uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

uint16_t mc_read_u16(const uint8_t* input) {
    return (uint16_t)((uint16_t)input[0] | (uint16_t)((uint16_t)input[1] << 8));
}

uint32_t mc_read_u32(const uint8_t* input) {
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) | ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

uint32_t mc_crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for(size_t i = 0U; i < size; i++) {
        crc ^= data[i];
        for(uint8_t bit = 0U; bit < 8U; bit++) {
            const uint32_t mask = (uint32_t) - (int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}
