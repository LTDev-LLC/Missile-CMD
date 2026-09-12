#pragma once

// Host-only controls for scripted input, fault injection, and final application observations

#include "app_model.h"
#include <input/input.h>

// Script timestamps use the frequency passed to host_inputs; key events are delivered in order
typedef struct {
    uint32_t tick;
    InputKey key;
    InputType type;
} HostInput;
// Clear virtual files, scripted input, injected failures, and observations between tests
void host_reset(void);
// Install timed input events and choose the virtual clock frequency for the next app run
void host_inputs(const HostInput* inputs, size_t count, uint32_t frequency);
// Schedule one virtual stall so the real loop can exercise its catch-up limit
void host_stall_at(uint32_t tick, uint32_t duration);
// Fail storage mutations after the requested number of calls; negative values disable faults
void host_fail_storage_after(int operation);
// Flip the final stored byte so recovery tests encounter a damaged checksum
void host_corrupt(const char* path);
// The viewport copies final UI state before teardown so tests can inspect the completed run
extern McUiModel host_final_ui;
extern uint32_t host_redraws;
// Record simulation ticks at up to four pause gestures to detect paused-time replay
extern uint32_t host_sim_at_pause[4];
extern uint32_t host_pause_count;
typedef enum {
    HostStorageOpen,
    HostStorageRead,
    HostStorageSeek,
    HostStorageWrite,
    HostStorageSync,
    HostStorageRename,
    HostStorageRemove,
    HostStorageStat,
    HostStorageMkdir,
    HostStorageDirOpen,
    HostStorageDirRead,
    HostStorageOperationCount
} HostStorageOperation;
// Fail the next matching storage operation once without affecting other operation types
void host_fail_next(HostStorageOperation operation);
// Skip successful matching operations before injecting one failure.
void host_fail_operation_after(HostStorageOperation operation, unsigned count);

void host_defer_io(bool defer);
void host_complete_io(void);

void host_wait_storage_blocked(void);
extern void (*host_render_hook)(void);

void host_load_fixture(const char* local, const char* remote);

extern unsigned host_random_calls;
