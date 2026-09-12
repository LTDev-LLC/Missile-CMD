#pragma once

// Firmware sequence symbols referenced by production feedback and supplied by the host stub

#include "notification.h"
#include <stddef.h>
extern const NotificationMessage message_delay_25, message_delay_50, message_delay_100,
    message_delay_250;
extern const NotificationMessage message_sound_off, message_vibro_on, message_vibro_off;
extern const NotificationMessage message_red_255, message_green_255;
extern const NotificationMessage message_red_0, message_green_0, message_blue_0;
extern const NotificationMessage message_blink_stop, message_do_not_reset;
extern const NotificationSequence sequence_single_vibro;
extern const NotificationSequence sequence_reset_rgb;
