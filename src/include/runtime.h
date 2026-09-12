#pragma once

#include "game.h"
#include "balance.h"
#include "persistence_codec.h"

#include <stdbool.h>
#include <stdint.h>

#define MC_MAX_CATCH_UP_STEPS 3U

// fraction is elapsed platform ticks times FPS, modulo the platform tick frequency
typedef struct {
    uint32_t tick;
    uint32_t fraction;
    uint8_t fps;
} McRenderClock;

static inline bool
    mc_render_clock_advance(McRenderClock* clock, uint32_t now, uint32_t frequency, uint8_t fps) {
    if(!clock->fps || !fps || !frequency) {
        *clock = (McRenderClock){.tick = now, .fps = fps};
        return false;
    }
    // Charge elapsed time at the old FPS to preserve frame phase across rate changes
    const uint64_t elapsed = clock->fraction + (uint64_t)(now - clock->tick) * clock->fps;
    clock->tick = now;
    clock->fps = fps;
    // Discard missed whole frames so they cannot build a redraw backlog
    clock->fraction = (uint32_t)(elapsed % frequency);
    return elapsed >= frequency;
}

static inline uint32_t
    mc_render_clock_wait(const McRenderClock* clock, uint32_t now, uint32_t frequency) {
    if(!clock->fps) return UINT32_MAX;
    const uint64_t elapsed = clock->fraction + (uint64_t)(now - clock->tick) * clock->fps;
    // Round up so fractional deadlines cannot wake the loop early
    return elapsed >= frequency ? 0U :
                                  (uint32_t)((frequency - elapsed + clock->fps - 1U) / clock->fps);
}

static inline uint8_t mc_runtime_steps(
    uint32_t* accumulator,
    uint32_t elapsed,
    uint32_t frequency,
    bool was_playing,
    bool playing) {
    // Clear backlog on pause transitions so menu time cannot advance the game
    if(!was_playing || !playing || frequency == 0U) {
        *accumulator = 0U;
        return 0U;
    }
    const uint64_t total = *accumulator + (uint64_t)elapsed * MC_TICKS_PER_SECOND;
    const uint64_t due = total / frequency;
    *accumulator = (uint32_t)(total % frequency);
    // Drop excess steps to keep input responsive after a stall
    return due > MC_MAX_CATCH_UP_STEPS ? MC_MAX_CATCH_UP_STEPS : (uint8_t)due;
}

static inline uint8_t mc_render_target_fps(const McGame* game, McRenderMode mode) {
    if(mode == McRenderBatterySaver) return mc_game_has_motion(game) ? 15U : 0U;
    if(game->interceptor_count > 0U || game->root_count > 0U || game->chain_count > 0U ||
       game->impact_count > 0U || game->enemy_count >= 4U)
        return 30U;
    return game->enemy_count > 0U ? 20U : 0U;
}
