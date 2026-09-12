#pragma once

#include "persistence.h"

#define MC_CLEANUP_ALLOCATION_BUDGET 4096U

typedef enum {
    McCleanupScanning = 0,
    McCleanupPrompt,
    McCleanupPurging,
    McCleanupFailed,
    McCleanupDone,
} McCleanupState;

typedef struct McCleanupFolder {
    struct McCleanupFolder* next;
    char name[];
} McCleanupFolder;

typedef struct {
    Storage* storage;
    File* scan;
    McCleanupFolder* folders;
    McCleanupFolder* tail;
    McCleanupFolder* target;
    size_t count, remaining, allocated, root_length;
    McCleanupState state;
    bool approved, limited;
    char path[512];
} McCleanup;

bool mc_cleanup_candidate(const char* name);
void mc_cleanup_init(McCleanup* cleanup, Storage* storage);
void mc_cleanup_deinit(McCleanup* cleanup);
void mc_cleanup_step(McCleanup* cleanup);
void mc_cleanup_approve(McCleanup* cleanup);
void mc_cleanup_retry(McCleanup* cleanup);
const char* mc_cleanup_name(const McCleanup* cleanup, size_t index);
