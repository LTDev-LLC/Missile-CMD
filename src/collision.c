#include "collision.h"

#include "balance.h"

#include <limits.h>

uint32_t mc_distance_squared(int16_t ax, int16_t ay, int16_t bx, int16_t by) {
    const int32_t dx = (int32_t)ax - bx;
    const int32_t dy = (int32_t)ay - by;
    return (uint32_t)(dx * dx + dy * dy);
}

// Integer base-four square root, rounded down
uint16_t mc_integer_sqrt(uint32_t value) {
    uint32_t result = 0U;
    // Start at the highest power of four; each iteration consumes two bits
    uint32_t bit = 1UL << 30;
    while(bit > value)
        bit >>= 2;
    while(bit != 0U) {
        if(value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint16_t)result;
}

uint16_t mc_path_duration(
    int16_t start_x,
    int16_t start_y,
    int16_t target_x,
    int16_t target_y,
    uint16_t speed_x100) {
    const uint16_t distance =
        mc_integer_sqrt(mc_distance_squared(start_x, start_y, target_x, target_y));
    if(distance == 0U || speed_x100 == 0U) return 1U;
    const uint32_t numerator = (uint32_t)distance * MC_TICKS_PER_SECOND * 100U;
    const uint32_t duration = (numerator + speed_x100 - 1U) / speed_x100;
    return duration == 0U ? 1U : duration > UINT16_MAX ? UINT16_MAX : (uint16_t)duration;
}

static int16_t mc_path_velocity(int32_t delta, uint16_t duration) {
    const int32_t half = (int32_t)duration / 2;
    // Round positive and negative velocities symmetrically
    const int32_t rounded = delta >= 0 ? delta + half : delta - half;
    const int32_t velocity = rounded / (int32_t)duration;
    return velocity > INT16_MAX ? INT16_MAX : velocity < INT16_MIN ? INT16_MIN : (int16_t)velocity;
}

void mc_path_init(
    McPath* path,
    int16_t start_x,
    int16_t start_y,
    int16_t target_x,
    int16_t target_y,
    uint16_t duration) {
    if(duration == 0U) duration = 1U;
    path->x_q9 = (uint16_t)((uint16_t)start_x << MC_PATH_SHIFT);
    path->y_q9 = (uint16_t)((uint16_t)start_y << MC_PATH_SHIFT);
    path->target_x = (uint8_t)target_x;
    path->target_y = (uint8_t)target_y;
    path->remaining = duration;
    const int32_t target_x_q9 = (int32_t)target_x << MC_PATH_SHIFT;
    const int32_t target_y_q9 = (int32_t)target_y << MC_PATH_SHIFT;
    path->vx_q9 = mc_path_velocity(target_x_q9 - path->x_q9, duration);
    path->vy_q9 = mc_path_velocity(target_y_q9 - path->y_q9, duration);
}

bool mc_path_step(McPath* path) {
    // Snap on arrival because rounded Q9 velocities can accumulate drift
    if(path->remaining <= 1U) {
        path->remaining = 0U;
        path->x_q9 = (uint16_t)((uint16_t)path->target_x << MC_PATH_SHIFT);
        path->y_q9 = (uint16_t)((uint16_t)path->target_y << MC_PATH_SHIFT);
        return true;
    }
    path->x_q9 = (uint16_t)((int32_t)path->x_q9 + path->vx_q9);
    path->y_q9 = (uint16_t)((int32_t)path->y_q9 + path->vy_q9);
    path->remaining--;
    return false;
}

uint8_t mc_path_x(const McPath* path) {
    return (uint8_t)(path->x_q9 >> MC_PATH_SHIFT);
}

uint8_t mc_path_y(const McPath* path) {
    return (uint8_t)(path->y_q9 >> MC_PATH_SHIFT);
}

bool mc_point_in_radius(int16_t px, int16_t py, int16_t cx, int16_t cy, uint8_t radius) {
    return mc_distance_squared(px, py, cx, cy) <= (uint32_t)radius * radius;
}
