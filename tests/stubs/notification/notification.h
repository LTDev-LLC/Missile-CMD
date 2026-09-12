#pragma once

// Notification payload shapes accepted by the silent host feedback stub

#include <stdbool.h>
#include <stdint.h>
#define RECORD_NOTIFICATION "notification"
typedef struct NotificationApp {
    int unused;
} NotificationApp;
typedef enum {
    NotificationMessageTypeSoundOn,
    NotificationMessageTypeSoundOff,
    NotificationMessageTypeVibro,
    NotificationMessageTypeLedRed,
    NotificationMessageTypeLedGreen,
    NotificationMessageTypeLedBlue,
    NotificationMessageTypeLedBlinkStop,
    NotificationMessageTypeDelay,
    NotificationMessageTypeDoNotReset,
} NotificationMessageType;
typedef struct {
    NotificationMessageType type;
    union {
        struct {
            float frequency;
            float volume;
        } sound;
        struct {
            uint8_t value;
        } led;
        struct {
            uint32_t length;
        } delay;
        struct {
            bool on;
        } vibro;
    } data;
} NotificationMessage;
// Sequences are null-terminated message-pointer arrays, matching production feedback tables
typedef const NotificationMessage* NotificationSequence[];
// Accept notification sequences silently because physical feedback is device-only
void notification_message(NotificationApp* app, const NotificationSequence* sequence);
// Use the same silent host notification handler without delaying virtual time
void notification_message_block(NotificationApp* app, const NotificationSequence* sequence);
