#pragma once

#include "persistence_codec.h"
#include "data_paths.h"

#include <storage/storage.h>

#define MC_PERSISTENCE_SCRATCH_SIZE  1024U
#define MC_HISTORY_ALLOCATION_BUDGET 512U

typedef enum {
    McStorageOk = 0,
    McStorageMissing,
    McStorageInvalid,
    McStorageIoError,
    McStorageBusy,
} McStorageResult;

typedef struct McPaceHistory McPaceHistory;

typedef struct {
    Storage* storage;
    uint8_t* scratch;
    // One bit per family; only a validated primary may rotate into the backup
    uint16_t trusted_primary;
    // Generation advances after saves; timestamp comes from slot restoration
    uint32_t generation;
    uint32_t timestamp;
    uint8_t slot;
    char pace_name[48];
    bool recovered;
    McPaceHistory* history;
} McPersistence;

bool mc_persistence_init(McPersistence* persistence, Storage* storage);
void mc_persistence_deinit(McPersistence* persistence);

// Loads fall back to backups, then defaults if neither copy is valid
McStorageResult mc_persistence_load_settings(McPersistence* persistence, McSettings* settings);
McStorageResult mc_persistence_load_scores(McPersistence* persistence, McScoreTables* scores);
McStorageResult mc_persistence_load_profile(McPersistence* persistence, McProfile* profile);
McStorageResult
    mc_persistence_save_settings(McPersistence* persistence, const McSettings* settings);
McStorageResult
    mc_persistence_save_scores(McPersistence* persistence, const McScoreTables* scores);
McStorageResult mc_persistence_save_profile(McPersistence* persistence, const McProfile* profile);
// Recognized root records are migrated through codecs, not copied as opaque files.
bool mc_persistence_migration_file(const char* name);
McStorageResult
    mc_persistence_migrate_settings(McPersistence* p, const char* source, McSettings* settings);
McStorageResult
    mc_persistence_migrate_scores(McPersistence* p, const char* source, McScoreTables* scores);
McStorageResult
    mc_persistence_migrate_profile(McPersistence* p, const char* source, McProfile* profile);
McStorageResult
    mc_persistence_migrate_run(McPersistence* p, const char* source, McRunSnapshot* run);
_Static_assert(
    MC_PERSISTENCE_SCRATCH_SIZE >= MC_RUN_ENCODED_MAX_SIZE,
    "persistence scratch must fit maximum run");

#define MC_SAVE_SLOTS 3U
typedef struct {
    uint32_t score;
    uint32_t generation;
    uint32_t timestamp;
    uint16_t wave;
    McGameMode mode;
    McDifficulty difficulty;
    uint8_t status; // McStorageResult
} McSaveInfo;
McStorageResult mc_persistence_checkpoint(McPersistence* p, const McRunSnapshot* run);
McStorageResult mc_persistence_restore(McPersistence* p, McRunSnapshot* run);
McStorageResult mc_persistence_remove_checkpoint(McPersistence* p);
McStorageResult mc_persistence_pace(McPersistence* p, const McGame* game, McPace* pace, bool save);
McStorageResult mc_persistence_history_step(McPersistence* p);
bool mc_persistence_history_clear(McPersistence* p);
bool mc_persistence_history_pending(const McPersistence* p);
// Discard optional in-memory maintenance before lending its heap allowance to version data.
bool mc_persistence_history_release(McPersistence* p);
void mc_persistence_slot_info(McPersistence* p, uint8_t slot, McSaveInfo* info);

// Must finish before replacing the current run or slot
bool mc_persistence_history_required(const McPersistence* p);
