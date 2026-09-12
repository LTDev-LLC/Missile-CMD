#include "feedback.h"

#include <notification/notification_messages.h>

#define MC_LED_INTERVAL_TICKS 6U

static const NotificationMessage McNoteLaunch = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 880.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteEmpty = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 220.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteChain1 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 660.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteChain2 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 784.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteChain3 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 988.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteLoss1 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 330.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteLoss2 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 196.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteClear1 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 523.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteOver1 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 392.0f, .volume = 1.0f},
};
static const NotificationMessage McNoteOver2 = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 262.0f, .volume = 1.0f},
};

static const NotificationSequence McSequenceLaunch = {
    &McNoteLaunch,
    &message_delay_25,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceLaunchVibro = {
    &message_vibro_on,
    &message_delay_25,
    &message_vibro_off,
    NULL,
};
static const NotificationSequence McSequenceLaunchBoth = {
    &message_vibro_on,
    &McNoteLaunch,
    &message_delay_25,
    &message_vibro_off,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceEmpty = {
    &McNoteEmpty,
    &message_delay_25,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceEmptyVibro = {
    &message_vibro_on,
    &message_delay_50,
    &message_vibro_off,
    NULL,
};
static const NotificationSequence McSequenceEmptyBoth = {
    &message_vibro_on,
    &McNoteEmpty,
    &message_delay_50,
    &message_vibro_off,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceChain1 = {
    &McNoteChain1,
    &message_delay_25,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceChain2 = {
    &McNoteChain2,
    &message_delay_25,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceChain3 = {
    &McNoteChain3,
    &message_delay_25,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceLossSound = {
    &McNoteLoss1,
    &message_delay_50,
    &McNoteLoss2,
    &message_delay_100,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceLossBoth = {
    &message_vibro_on,
    &McNoteLoss1,
    &message_delay_50,
    &McNoteLoss2,
    &message_delay_50,
    &message_vibro_off,
    &message_delay_50,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceClear = {
    &McNoteClear1,
    &message_delay_50,
    &McNoteChain2,
    &message_delay_100,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceClearBoth = {
    &message_vibro_on,
    &McNoteClear1,
    &message_delay_50,
    &McNoteChain2,
    &message_delay_50,
    &message_vibro_off,
    &message_delay_50,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceGameOver = {
    &McNoteOver1,
    &message_delay_100,
    &McNoteOver2,
    &message_delay_250,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceGameOverBoth = {
    &message_vibro_on,
    &McNoteOver1,
    &message_delay_100,
    &message_vibro_off,
    &McNoteOver2,
    &message_delay_250,
    &message_sound_off,
    NULL,
};
static const NotificationSequence McSequenceSoundPreview = {
    &McNoteLaunch,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

// Explicit off values and dark intervals prevent the charging color returning between cues
static const NotificationSequence McSequenceLedYellow = {
    &message_red_255,
    &message_green_255,
    &message_delay_50,
    &message_red_0,
    &message_green_0,
    &message_delay_50,
    &message_do_not_reset,
    NULL,
};
static const NotificationSequence McSequenceLedRed = {
    &message_red_255,
    &message_delay_100,
    &message_red_0,
    &message_delay_100,
    &message_do_not_reset,
    NULL,
};
static const NotificationSequence McSequenceLedGreen = {
    &message_green_255,
    &message_delay_100,
    &message_green_0,
    &message_delay_100,
    &message_do_not_reset,
    NULL,
};
static const NotificationSequence McSequenceStop = {
    &message_blink_stop,
    &message_red_0,
    &message_green_0,
    &message_blue_0,
    &message_sound_off,
    &message_vibro_off,
    &message_do_not_reset,
    NULL,
};

void mc_feedback_init(
    McFeedback* feedback,
    NotificationApp* notification,
    const McSettings* settings) {
    feedback->notification = notification;
    feedback->settings = settings;
    feedback->last_chain_tick = 0U;
    feedback->last_led_tick = 0U;
    mc_feedback_stop(feedback);
}

static void mc_feedback_send(
    McFeedback* feedback,
    const NotificationSequence* sound,
    const NotificationSequence* vibration,
    const NotificationSequence* both) {
    if(feedback->settings->vibration && feedback->settings->vibration_intensity == 0U) {
        if(feedback->settings->sound) notification_message(feedback->notification, sound);
        notification_message(feedback->notification, &McSequenceLaunchVibro);
        return;
    }
    if(feedback->settings->sound && feedback->settings->vibration) {
        notification_message(feedback->notification, both);
    } else if(feedback->settings->sound) {
        notification_message(feedback->notification, sound);
    } else if(feedback->settings->vibration) {
        notification_message(feedback->notification, vibration);
    }
}

static void mc_feedback_sound(McFeedback* feedback, const NotificationSequence* sequence) {
    if(feedback->settings->sound) notification_message(feedback->notification, sequence);
}

void mc_feedback_launch(McFeedback* feedback) {
    mc_feedback_send(feedback, &McSequenceLaunch, &McSequenceLaunchVibro, &McSequenceLaunchBoth);
}

void mc_feedback_empty(McFeedback* feedback) {
    mc_feedback_send(feedback, &McSequenceEmpty, &McSequenceEmptyVibro, &McSequenceEmptyBoth);
}

void mc_feedback_led(McFeedback* feedback, uint32_t tick, McLedCue cue) {
    if(feedback->settings->led_mode == 2U ||
       (feedback->settings->led_mode == 1U && cue < McLedSiteLost))
        return;
    if(cue == McLedNone) return;
    // Higher-priority alerts may interrupt the quiet interval
    if(tick - feedback->last_led_tick < feedback->led_quiet_ticks && cue <= feedback->led_cue)
        return;
    feedback->last_led_tick = tick;
    feedback->led_cue = cue;
    const uint8_t flashes = cue == McLedExplosion ? 1U : cue == McLedGameOver ? 3U : 2U;
    feedback->led_quiet_ticks = MC_LED_INTERVAL_TICKS * flashes;
    const NotificationSequence* sequence = &McSequenceLedRed;
    if(cue == McLedExplosion) sequence = &McSequenceLedYellow;
    if(cue == McLedWaveClear) sequence = &McSequenceLedGreen;
    for(uint8_t i = 0U; i < flashes; i++)
        notification_message(feedback->notification, sequence);
}

void mc_feedback_chain(McFeedback* feedback, uint32_t tick, uint8_t chain_depth) {
    // Space chain tones by three ticks to avoid flooding the notification queue
    if(!feedback->settings->sound || (tick - feedback->last_chain_tick) < 3U) return;
    feedback->last_chain_tick = tick;
    const NotificationSequence* sequence = &McSequenceChain1;
    if(chain_depth >= 4U) {
        sequence = &McSequenceChain3;
    } else if(chain_depth >= 2U) {
        sequence = &McSequenceChain2;
    }
    notification_message(feedback->notification, sequence);
}

void mc_feedback_site_lost(McFeedback* feedback) {
    mc_feedback_send(feedback, &McSequenceLossSound, &sequence_single_vibro, &McSequenceLossBoth);
}

void mc_feedback_wave_clear(McFeedback* feedback) {
    mc_feedback_send(feedback, &McSequenceClear, &sequence_single_vibro, &McSequenceClearBoth);
}

void mc_feedback_game_over(McFeedback* feedback) {
    mc_feedback_send(
        feedback, &McSequenceGameOver, &sequence_single_vibro, &McSequenceGameOverBoth);
}

void mc_feedback_preview_sound(McFeedback* feedback) {
    mc_feedback_sound(feedback, &McSequenceSoundPreview);
}

void mc_feedback_preview_vibration(McFeedback* feedback) {
    if(feedback->settings->vibration) {
        const NotificationSequence* sequence = &McSequenceLaunchVibro;
        if(feedback->settings->vibration_intensity) sequence = &sequence_single_vibro;
        notification_message(feedback->notification, sequence);
    }
}

void mc_feedback_stop(McFeedback* feedback) {
    notification_message_block(feedback->notification, &McSequenceStop);
    feedback->led_quiet_ticks = 0U;
    feedback->led_cue = McLedNone;
}

void mc_feedback_deinit(McFeedback* feedback) {
    mc_feedback_stop(feedback);
    notification_message_block(feedback->notification, &sequence_reset_rgb);
}
