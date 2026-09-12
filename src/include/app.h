#pragma once

#include <stdint.h>

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((externally_visible))
#endif
int32_t
    mc_app_run(void);
