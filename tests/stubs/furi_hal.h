#pragma once

// Fixed hardware seed and timestamp providers for reproducible host runs

#include <stdint.h>
// Return a fixed seed so setup and gameplay captures are reproducible
uint32_t furi_hal_random_get(void);
// Return the fixed host timestamp used by save metadata and Daily setup
uint32_t furi_hal_rtc_get_timestamp(void);
