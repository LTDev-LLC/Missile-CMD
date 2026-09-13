#pragma once

#include "storage_cleanup.h"
#include "help.h"
#include <furi.h>
#include <stdatomic.h>

#define MC_WAKE_INPUT    1U
#define MC_WAKE_IO       2U
#define MC_IO_STACK_SIZE 4096U
enum {
    McIoIdle,
    McIoPending,
    McIoComplete,
    McIoStopping,
    McIoStopped
};
typedef uint8_t McIoOperation;
enum {
    McIoLoadSettings,
    McIoLoadScores,
    McIoLoadProfile,
    McIoLoadRun,
    McIoSaveRun,
    McIoDeleteRun,
    McIoLoadPace,
    McIoSavePace,
    McIoSlots,
    McIoSaveSettings,
    McIoSaveScores,
    McIoSaveProfile,
    McIoHistory,
    McIoCleanupInit,
    McIoCleanupStep,
    McIoHelp,
};
typedef struct {
    size_t index, count, remaining, requested_index;
    uint16_t offset, length, requested_offset;
    uint8_t action;
    McCleanupState state;
    bool approved, limited, can_migrate, migrating, imported, manual;
    char name[41];
    char source[41];
} McCleanupReply;
typedef struct {
    McIoOperation operation;
    uint8_t slot, startup;
    uint16_t family;
    uint32_t generation, timestamp, revision, run_identity;
    McStorageResult result;
    bool recovered, history_pending, history_required, return_title;
    union {
        McHelpPage help;
        McSettings settings;
        McScoreTables scores;
        McProfile profile;
        struct {
            McGame game;
            McWaveStart wave;
            McRunStage stage;
        } run;
        struct {
            McGame game;
            McPace pace;
        } pace;
        McSaveInfo info;
        McCleanupReply cleanup;
    } data;
} McIoJob;
typedef struct {
    McIoJob job;
    McPersistence* persistence;
    McCleanup* cleanup;
    FuriThread* thread;
    FuriThreadId owner;
    atomic_uint state;
} McStorageWorker;

void mc_storage_worker_init(
    McStorageWorker* worker,
    McPersistence* persistence,
    McCleanup* cleanup);
void mc_storage_worker_submit(McStorageWorker* worker);
// Runs one immutable request. Shared by the device thread and deterministic host scheduler.
void mc_storage_worker_execute(McStorageWorker* worker);
void mc_storage_worker_finish(McStorageWorker* worker);
void mc_storage_worker_stop(McStorageWorker* worker);
void mc_storage_worker_free(McStorageWorker* worker);
