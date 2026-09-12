#include "controls.h"

uint16_t mc_cursor_base_step_q8(McCursorSpeed speed) {
    static const uint16_t BaseStepQ8[McCursorCount] = {115U, 173U, 230U};
    if(speed >= McCursorCount) speed = McCursorNormal;
    return BaseStepQ8[speed];
}

uint16_t mc_cursor_step_q8(McCursorSpeed speed, uint16_t held_ticks, bool diagonal) {
    uint32_t step = mc_cursor_base_step_q8(speed);
    if(held_ticks >= 20U) {
        step *= 2U;
    } else if(held_ticks >= 8U) {
        step = (step * 3U) / 2U;
    }
    // 181/256 approximates 1/sqrt(2) to normalize diagonal speed
    if(diagonal) step = (step * 181U) / 256U;
    return (uint16_t)step;
}

uint8_t mc_wrap_increment(uint8_t value, uint8_t count) {
    if(count == 0U) return 0U;
    return (uint8_t)((value + 1U) % count);
}

uint8_t mc_wrap_decrement(uint8_t value, uint8_t count) {
    if(count == 0U) return 0U;
    return (value == 0U || value >= count) ? (uint8_t)(count - 1U) : (uint8_t)(value - 1U);
}

uint8_t mc_wrap_step(uint8_t value, uint8_t count, bool increment) {
    return increment ? mc_wrap_increment(value, count) : mc_wrap_decrement(value, count);
}
uint8_t mc_list_first(uint8_t selected, uint8_t visible) {
    return visible && selected >= visible ? selected - visible + 1U : 0U;
}
