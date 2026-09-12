#pragma once

// Minimal firmware runtime declarations implemented by the single-threaded host harness

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define UNUSED(x)       (void)(x)
#define furi_check(x)   assert(x)
#define FuriWaitForever UINT32_MAX
typedef enum {
    FuriStatusOk,
    FuriStatusErrorTimeout,
    FuriStatusErrorResource
} FuriStatus;
typedef enum {
    FuriMutexTypeNormal
} FuriMutexType;
typedef struct FuriMutex FuriMutex;
typedef struct FuriMessageQueue FuriMessageQueue;
// Allocate a lock-state checker for the single-threaded host harness
FuriMutex* furi_mutex_alloc(FuriMutexType type);
// Release a host mutex only after its final unlock
void furi_mutex_free(FuriMutex* p);
// Reject recursive acquisition instead of blocking in the single-threaded harness
FuriStatus furi_mutex_acquire(FuriMutex* p, uint32_t timeout);
// Reject an unmatched unlock and make the host mutex available again
FuriStatus furi_mutex_release(FuriMutex* p);
// Allocate a bounded host queue after checking that its items are input events
FuriMessageQueue* furi_message_queue_alloc(uint32_t count, uint32_t size);
// Release the host queue after the application has stopped using it
void furi_message_queue_free(FuriMessageQueue* p);
// Enqueue without waiting, reporting a full queue after sixteen buffered events
FuriStatus furi_message_queue_put(FuriMessageQueue* p, const void* msg, uint32_t timeout);
// Deliver queued or scripted input, advancing virtual time through waits and stalls
FuriStatus furi_message_queue_get(FuriMessageQueue* p, void* msg, uint32_t timeout);
// Discard queued events without altering the timed input script
FuriStatus furi_message_queue_reset(FuriMessageQueue* p);
// Read the virtual clock advanced by input waits rather than real wall-clock time
uint32_t furi_get_tick(void);
// Convert milliseconds at the configured host frequency using wide intermediate arithmetic
uint32_t furi_ms_to_ticks(uint32_t ms);
// Return the tick frequency selected for the current scripted run
uint32_t furi_kernel_get_tick_frequency(void);
// Return stable in-memory placeholders for storage, GUI, and notification services
void* furi_record_open(const char* name);
// Accept service release without freeing the static host placeholders
void furi_record_close(const char* name);

#define FuriFlagWaitAny 0U
typedef struct FuriThread FuriThread;
typedef FuriThread* FuriThreadId;
typedef int32_t (*FuriThreadCallback)(void*);
FuriThread* furi_thread_alloc_ex(const char*, uint32_t, FuriThreadCallback, void*);
void furi_thread_start(FuriThread*);
bool furi_thread_join(FuriThread*);
void furi_thread_free(FuriThread*);
FuriThreadId furi_thread_get_id(FuriThread*);
FuriThreadId furi_thread_get_current_id(void);
uint32_t furi_thread_flags_set(FuriThreadId, uint32_t);
uint32_t furi_thread_flags_wait(uint32_t, uint32_t, uint32_t);
uint32_t furi_message_queue_get_space(FuriMessageQueue*);
