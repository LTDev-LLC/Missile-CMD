#pragma once

// Input event shapes and key ordering used by the real application controller

#include <stdint.h>
// Keep firmware key ordinals because the application stores physical keys in a bitmask
typedef enum {
    InputKeyUp,
    InputKeyDown,
    InputKeyRight,
    InputKeyLeft,
    InputKeyOk,
    InputKeyBack
} InputKey;
typedef enum {
    InputTypePress,
    InputTypeRelease,
    InputTypeShort,
    InputTypeLong,
    InputTypeRepeat
} InputType;
typedef struct {
    InputKey key;
    InputType type;
    uint32_t sequence;
} InputEvent;
