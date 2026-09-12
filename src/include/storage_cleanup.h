#pragma once

#include "persistence.h"

#define MC_CLEANUP_ALLOCATION_BUDGET 4096U

typedef enum {
    McCleanupScanning = 0,
    McCleanupPrompt,
    McCleanupPurging,
    McCleanupFailed,
    McCleanupDone,
    McCleanupMigrating,
    McCleanupValidating,
} McCleanupState;

typedef struct McCleanupFolder {
    struct McCleanupFolder* next;
    char name[];
} McCleanupFolder;

typedef struct {
    Storage* storage;
    File* scan;
    File* input;
    File* output;
    McCleanupFolder* folders;
    McCleanupFolder* tail;
    McCleanupFolder* target;
    McCleanupFolder* source;
    size_t count, remaining, allocated, root_length;
    McCleanupState state;
    McStorageResult result;
    bool approved, limited;
    bool selecting, migrating, migrated, resuming, verifying;
    uint8_t validation;
    char path[512];
} McCleanup;

bool mc_cleanup_candidate(const char* name);
// SemVer precedence (build metadata breaks ties only when choosing a source).
int mc_cleanup_version_compare(const char* left, const char* right);
void mc_cleanup_init(McCleanup* cleanup, Storage* storage);
void mc_cleanup_deinit(McCleanup* cleanup);
void mc_cleanup_step(McCleanup* cleanup);
void mc_cleanup_approve(McCleanup* cleanup);
void mc_cleanup_migrate(McCleanup* cleanup);
void mc_cleanup_migration_step(McCleanup* cleanup, uint8_t* scratch);
void mc_cleanup_validated(McCleanup* cleanup, McStorageResult result);
void mc_cleanup_retry(McCleanup* cleanup);
const char* mc_cleanup_name(const McCleanup* cleanup, size_t index);
