#pragma once

#include "persistence_codec.h"

#include <notification/notification.h>

// Higher-priority alerts replace ordinary explosion cues from the same tick
typedef uint8_t McLedCue;
enum {
    McLedNone = 0,
    McLedExplosion,
    McLedWaveClear,
    McLedSiteLost,
    McLedGameOver,
};

// Borrows settings and the notification handle; both must outlive this object
typedef struct {
    NotificationApp* notification;
    const McSettings* settings;
    uint32_t last_chain_tick;
    uint32_t last_led_tick;
    uint8_t led_quiet_ticks;
    McLedCue led_cue;
} McFeedback;

void mc_feedback_init(
    McFeedback* feedback,
    NotificationApp* notification,
    const McSettings* settings);
void mc_feedback_launch(McFeedback* feedback);
void mc_feedback_empty(McFeedback* feedback);
void mc_feedback_led(McFeedback* feedback, uint32_t tick, McLedCue cue);
void mc_feedback_chain(McFeedback* feedback, uint32_t tick, uint8_t chain_depth);
void mc_feedback_site_lost(McFeedback* feedback);
void mc_feedback_wave_clear(McFeedback* feedback);
void mc_feedback_game_over(McFeedback* feedback);
void mc_feedback_preview_sound(McFeedback* feedback);
void mc_feedback_preview_vibration(McFeedback* feedback);
// Wait for pending cues and keep sound, vibration, and LED off
void mc_feedback_stop(McFeedback* feedback);
// Wait for app feedback, then return LED control to firmware
void mc_feedback_deinit(McFeedback* feedback);
