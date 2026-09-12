#pragma once

// Minimal display-date fields supplied by the deterministic host date converter

#include <stdint.h>
typedef struct {
    uint16_t year;
    uint8_t month, day;
} DateTime;
// Return a fixed display date so screenshots do not depend on calendar conversion
void datetime_timestamp_to_datetime(uint32_t stamp, DateTime* date);
