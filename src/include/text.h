#pragma once

#include <stdint.h>
#include <string.h>

// Packed strings save one relocation per label. The caller must bound index
static inline const char* mc_text_at(const char* text, uint8_t index) {
    while(index--)
        text += strlen(text) + 1U;
    return text;
}
