#pragma once
#include "help_catalog.h"
#include "data_paths.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define MC_HELP_PAGE_BYTES   128U
#define MC_HELP_HEADER_BYTES 12U
#define MC_HELP_ENTRY_BYTES  8U
typedef uint8_t McHelpStatus;
enum {
    McHelpEmpty,
    McHelpLoading,
    McHelpReady,
    McHelpUnavailable
};
typedef struct {
    uint32_t revision; // Distinguish a reopened page from an older in-flight read.
    uint8_t id, status, lines;
    char text[MC_HELP_PAGE_BYTES];
} McHelpPage;
bool mc_help_header_valid(const uint8_t* header, size_t size);
bool mc_help_page_valid(const uint8_t* entry, McHelpPage* page);
