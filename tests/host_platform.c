// Virtual firmware services and observations shared by application tests and screenshots
#include "host_platform.h"
#include "app_internal.h"
#include <datetime/datetime.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef MC_HOST_THREADS
#include <pthread.h>
#include <time.h>
#include <errno.h>
#endif

// Lock ownership assertions model misuse without introducing real host threads
static _Thread_local unsigned host_held_locks;
struct FuriMutex {
    bool held;
#ifdef MC_HOST_THREADS
    pthread_mutex_t mutex;
#endif
};
struct FuriMessageQueue {
    InputEvent entries[32];
    uint8_t count, capacity;
};
// Fixed backing slots bound the in-memory filesystem and allow deleted entries to be reused
typedef struct {
    char path[512];
    uint8_t data[8192U];
    size_t size;
    bool exists, directory;
} HostFile;
static HostFile files[256];
struct File {
    HostFile* data;
    // File reads use a byte offset; directory reads use an index into the backing slot table
    size_t position;
    FS_Error error;
};
#ifdef MC_HOST_THREADS
static void host_storage_barrier(void);
#endif
static int fail_after = -1;
static int fail_operation = -1;
static unsigned fail_operation_skip;
static uint32_t ticks, frequency = 1000U;
static uint32_t timestamp = 1788480000U;
static uint32_t stall_tick, stall_duration;
static const HostInput* script;
static size_t input_count, input_index;
static ViewPort* viewport;
McUiModel host_final_ui;
uint32_t host_redraws, host_sim_at_pause[4], host_pause_count;

// Clear virtual files, scripted input, injected failures, and observations between tests
static void host_reset_threads(void);
unsigned host_random_calls;
unsigned mc_host_render_refreshes;
size_t mc_host_snapshot_bytes;
unsigned host_storage_calls[HostStorageOperationCount];
void host_reset(void) {
    host_random_calls = mc_host_render_refreshes = 0;
    mc_host_snapshot_bytes = 0;
    memset(host_storage_calls, 0, sizeof(host_storage_calls));
    host_reset_threads();
    memset(files, 0, sizeof(files));
    files[0].exists = files[0].directory = true;
    strcpy(files[0].path, "/data");
    fail_operation = -1;
    timestamp = 1788480000U;
    memset(&host_final_ui, 0, sizeof(host_final_ui));
    ticks = host_redraws = host_pause_count = 0U;
    stall_duration = 0U;
    fail_after = -1;
    script = NULL;
    input_count = input_index = 0U;
    viewport = NULL;
}
// Schedule one virtual stall so the real loop can exercise its catch-up limit
void host_stall_at(uint32_t tick, uint32_t duration) {
    stall_tick = tick;
    stall_duration = duration;
}
// Install timed input events and choose the virtual clock frequency for the next app run
void host_inputs(const HostInput* inputs, size_t count, uint32_t hz) {
    script = inputs;
    input_count = count;
    input_index = 0U;
    frequency = hz;
    ticks = host_redraws = host_pause_count = 0U;
}
// Fail storage mutations after the requested number of calls; negative values disable faults
void host_fail_storage_after(int operation) {
    fail_after = operation;
}
// Consume the mutation countdown and keep failing once it reaches zero
static bool fail(void) {
    if(fail_after < 0) return false;
    if(fail_after == 0) return true;
    fail_after--;
    return false;
}
// Fail the next matching storage operation once without affecting other operation types
void host_fail_next(HostStorageOperation operation) {
    host_fail_operation_after(operation, 0);
}
void host_fail_operation_after(HostStorageOperation operation, unsigned count) {
    fail_operation = operation;
    fail_operation_skip = count;
}
// Consume a one-shot fault only when the requested storage operation matches
static bool fail_next(HostStorageOperation operation) {
    host_storage_calls[operation]++;
    if(fail_operation != (int)operation) return false;
    if(fail_operation_skip) {
        fail_operation_skip--;
        return false;
    }
    fail_operation = -1;
    return true;
}
// Find a virtual path or allocate an unused backing-file slot when creation is requested
static HostFile* lookup(const char* path, bool create) {
    for(size_t i = 0; i < 256U; i++)
        if(files[i].exists && strcmp(files[i].path, path) == 0) return &files[i];
    if(create)
        for(size_t i = 0; i < 256U; i++)
            if(!files[i].exists) {
                memset(&files[i], 0, sizeof(files[i]));
                assert(strlen(path) < sizeof(files[i].path));
                strcpy(files[i].path, path);
                files[i].exists = true;
                return &files[i];
            }
    return NULL;
}
// Flip the final stored byte so recovery tests encounter a damaged checksum
void host_corrupt(const char* path) {
    HostFile* f = lookup(path, false);
    assert(f && f->size);
    f->data[f->size - 1U] ^= 1U;
}
// Report virtual regular files only, excluding directory entries
bool storage_file_exists(Storage* s, const char* path) {
    (void)s;
    HostFile* file = lookup(path, false);
    return file && !file->directory;
}
// Allocate an independent cursor and error state for a virtual file handle
File* storage_file_alloc(Storage* s) {
    (void)s;
    return calloc(1U, sizeof(File));
}
// Release the handle while leaving its backing data in the virtual filesystem
void storage_file_free(File* f) {
    free(f);
}
// Open a virtual file, truncate on creation, and honor injected open or mutation failures
bool storage_file_open(File* f, const char* path, FS_AccessMode access, FS_OpenMode mode) {
    (void)access;
    if(fail_next(HostStorageOpen) || (mode == FSOM_CREATE_ALWAYS && fail())) {
        f->error = FSE_INTERNAL;
        return false;
    }
    f->position = 0U;
    f->data = lookup(path, mode == FSOM_CREATE_ALWAYS);
    if(f->data && f->data->directory) f->data = NULL;
    f->error = f->data ? FSE_OK : FSE_NOT_EXIST;
    if(f->data && mode == FSOM_CREATE_ALWAYS) f->data->size = 0U;
    return f->data != NULL;
}
// Accept a file close without changing its retained in-memory contents
bool storage_file_close(File* f) {
    (void)f;
    return true;
}
// Return the backing file length in bytes
uint64_t storage_file_size(File* f) {
    return f->data->size;
}
// Read up to the remaining bytes, advancing the cursor unless a read fault is injected
size_t storage_file_read(File* f, void* data, size_t size) {
#ifdef MC_HOST_THREADS
    host_storage_barrier();
#endif
    if(fail_next(HostStorageRead)) {
        f->error = FSE_INTERNAL;
        return 0U;
    }
    if(size > f->data->size - f->position) size = f->data->size - f->position;
    memcpy(data, f->data->data + f->position, size);
    f->position += size;
    return size;
}
// Write within the fixed backing buffer, simulating a short write on injected failure
size_t storage_file_write(File* f, const void* data, size_t size) {
#ifdef MC_HOST_THREADS
    host_storage_barrier();
#endif
    assert(f->position + size <= sizeof(f->data->data));
    // A short write leaves partial temporary data for the production recovery path to handle
    if(fail_next(HostStorageWrite) || fail()) size /= 2U;
    memcpy(f->data->data + f->position, data, size);
    f->position += size;
    if(f->position > f->data->size) f->data->size = f->position;
    return size;
}
// Simulate a successful sync unless a one-shot or mutation-countdown fault applies
bool storage_file_sync(File* f) {
    (void)f;
    return !fail_next(HostStorageSync) && !fail();
}
// Copy a virtual file to its destination and retire the source after fault checks
FS_Error storage_common_rename(Storage* s, const char* src, const char* dest) {
    (void)s;
    if(fail_next(HostStorageRename) || fail()) return FSE_INTERNAL;
    HostFile* source = lookup(src, false);
    if(!source) return FSE_NOT_EXIST;
    HostFile* destination = lookup(dest, true);
    assert(destination);
    memcpy(destination->data, source->data, source->size);
    destination->size = source->size;
    source->exists = false;
    return FSE_OK;
}
// Remove a virtual entry while rejecting nonempty directories and injected failures
FS_Error storage_common_remove(Storage* s, const char* path) {
    (void)s;
    if(fail_next(HostStorageRemove) || fail()) return FSE_INTERNAL;
    HostFile* f = lookup(path, false);
    if(!f) return FSE_NOT_EXIST;
    if(f->directory) {
        size_t length = strlen(path);
        for(size_t i = 0U; i < 256U; i++)
            if(files[i].exists && !strncmp(files[i].path, path, length) &&
               files[i].path[length] == '/')
                return FSE_DENIED;
    }
    f->exists = false;
    return FSE_OK;
}

// Test the directory flag populated by the host storage stubs
bool file_info_is_dir(const FileInfo* info) {
    return (info->flags & FSF_DIRECTORY) != 0U;
}
// Return virtual entry metadata without opening a file handle
FS_Error storage_common_stat(Storage* s, const char* path, FileInfo* info) {
    (void)s;
    if(fail_next(HostStorageStat)) return FSE_INTERNAL;
    HostFile* file = lookup(path, false);
    if(!file) return FSE_NOT_EXIST;
    if(info) *info = (FileInfo){.flags = file->directory ? FSF_DIRECTORY : 0U, .size = file->size};
    return FSE_OK;
}
// Create a virtual directory, reporting existing paths and injected failures
FS_Error storage_common_mkdir(Storage* s, const char* path) {
    (void)s;
    if(fail_next(HostStorageMkdir) || fail()) return FSE_INTERNAL;
    if(lookup(path, false)) return FSE_EXIST;
    HostFile* file = lookup(path, true);
    if(!file) return FSE_INTERNAL;
    file->directory = true;
    return FSE_OK;
}
// Open a virtual directory and preserve the device restriction on trailing slashes
bool storage_dir_open(File* file, const char* path) {
    if(fail_next(HostStorageDirOpen)) {
        file->error = FSE_INTERNAL;
        return false;
    }
    // The device rejects directory paths with a trailing slash; do not hide that in tests
    const size_t length = strlen(path);
    if(length > 1U && path[length - 1U] == '/') {
        file->error = FSE_INVALID_NAME;
        return false;
    }
    file->data = lookup(path, false);
    file->position = 0U;
    if(!file->data || !file->data->directory) {
        file->error = FSE_NOT_EXIST;
        return false;
    }
    file->error = FSE_OK;
    return true;
}
// Detach the directory backing entry from its reusable scan handle
bool storage_dir_close(File* file) {
    file->data = NULL;
    return true;
}
// Visit direct children only, reporting end-of-directory separately from injected errors
bool storage_dir_read(File* file, FileInfo* info, char* name, uint16_t capacity) {
    if(fail_next(HostStorageDirRead)) {
        file->error = FSE_INTERNAL;
        return false;
    }
    assert(file->data && file->data->directory);
    const size_t prefix = strlen(file->data->path);
    while(file->position < 256U) {
        HostFile* entry = &files[file->position++];
        if(!entry->exists || strncmp(entry->path, file->data->path, prefix) ||
           entry->path[prefix] != '/')
            continue;
        const char* basename = entry->path + prefix + 1U;
        // Skip descendants of child folders so each scan exposes only immediate directory entries
        if(strchr(basename, '/')) continue;
        assert(strlen(basename) < capacity);
        strcpy(name, basename);
        *info = (FileInfo){.flags = entry->directory ? FSF_DIRECTORY : 0U, .size = entry->size};
        file->error = FSE_OK;
        return true;
    }
    file->error = FSE_NOT_EXIST;
    return false;
}
// Return the last error recorded on the virtual file or directory handle
FS_Error storage_file_get_error(File* file) {
    return file->error;
}

// Allocate a lock-state checker for the single-threaded host harness
FuriMutex* furi_mutex_alloc(FuriMutexType type) {
    (void)type;
    FuriMutex* m = calloc(1U, sizeof(FuriMutex));
#ifdef MC_HOST_THREADS
    pthread_mutex_init(&m->mutex, NULL);
#endif
    return m;
}
// Release a host mutex only after its final unlock
void furi_mutex_free(FuriMutex* m) {
    assert(!m->held);
#ifdef MC_HOST_THREADS
    pthread_mutex_destroy(&m->mutex);
#endif
    free(m);
}
// Reject recursive acquisition instead of blocking in the single-threaded harness
FuriStatus furi_mutex_acquire(FuriMutex* m, uint32_t timeout) {
    (void)timeout;
    assert(m);
#ifdef MC_HOST_THREADS
    pthread_mutex_lock(&m->mutex);
#else
    assert(!m->held);
#endif
    host_held_locks++;
    m->held = true;
    return FuriStatusOk;
}
// Reject an unmatched unlock and make the host mutex available again
FuriStatus furi_mutex_release(FuriMutex* m) {
    assert(m->held);
    m->held = false;
    host_held_locks--;
#ifdef MC_HOST_THREADS
    pthread_mutex_unlock(&m->mutex);
#endif
    return FuriStatusOk;
}
// Allocate a bounded host queue after checking that its items are input events
FuriMessageQueue* furi_message_queue_alloc(uint32_t count, uint32_t size) {
    assert(count <= 32U && size == sizeof(InputEvent));
    FuriMessageQueue* q = calloc(1U, sizeof(FuriMessageQueue));
    q->capacity = count;
    return q;
}
// Release the host queue after the application has stopped using it
void furi_message_queue_free(FuriMessageQueue* p) {
    free(p);
}
// Enqueue without waiting, reporting a full queue after sixteen buffered events
FuriStatus furi_message_queue_put(FuriMessageQueue* q, const void* event, uint32_t timeout) {
    (void)timeout;
    if(q->count == q->capacity) return FuriStatusErrorResource;
    q->entries[q->count++] = *(const InputEvent*)event;
    return FuriStatusOk;
}
// Discard queued events without altering the timed input script
FuriStatus furi_message_queue_reset(FuriMessageQueue* q) {
    q->count = 0U;
    return FuriStatusOk;
}
// Deliver queued or scripted input, advancing virtual time through waits and stalls
FuriStatus furi_message_queue_get(FuriMessageQueue* q, void* event, uint32_t timeout) {
    if(q->count) {
        *(InputEvent*)event = q->entries[0];
        memmove(q->entries, q->entries + 1U, --q->count * sizeof(InputEvent));
        return FuriStatusOk;
    }
    (void)timeout;
    return FuriStatusErrorTimeout;
}
uint32_t furi_message_queue_get_space(FuriMessageQueue* q) {
    return q->capacity - q->count;
}

#ifdef MC_HOST_THREADS
struct FuriThread {
    FuriThreadCallback callback;
    void* context;
    uint32_t flags;
    pthread_t native;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
};
static struct FuriThread main_thread = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER};
static _Thread_local FuriThread* current_thread;
static pthread_mutex_t storage_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t storage_condition = PTHREAD_COND_INITIALIZER;
static bool storage_hold, storage_blocked;
static void host_reset_threads(void) {
    main_thread.flags = 0;
    storage_hold = storage_blocked = false;
}
void host_defer_io(bool defer) {
    pthread_mutex_lock(&storage_gate);
    storage_hold = defer;
    pthread_cond_broadcast(&storage_condition);
    pthread_mutex_unlock(&storage_gate);
}
void host_complete_io(void) {
    host_defer_io(false);
}
static void host_storage_barrier(void) {
    pthread_mutex_lock(&storage_gate);
    while(storage_hold) {
        storage_blocked = true;
        pthread_cond_broadcast(&storage_condition);
        pthread_cond_wait(&storage_condition, &storage_gate);
    }
    storage_blocked = false;
    pthread_mutex_unlock(&storage_gate);
}
void host_wait_storage_blocked(void) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&storage_gate);
    while(!storage_blocked)
        assert(pthread_cond_timedwait(&storage_condition, &storage_gate, &until) == 0);
    pthread_mutex_unlock(&storage_gate);
}
static void* host_thread_main(void* data) {
    FuriThread* t = data;
    current_thread = t;
    t->callback(t->context);
    return NULL;
}
FuriThread*
    furi_thread_alloc_ex(const char* name, uint32_t stack, FuriThreadCallback cb, void* context) {
    (void)name;
    assert(stack == MC_IO_STACK_SIZE);
    FuriThread* t = calloc(1, sizeof(*t));
    t->callback = cb;
    t->context = context;
    pthread_mutex_init(&t->mutex, NULL);
    pthread_cond_init(&t->condition, NULL);
    return t;
}
void furi_thread_start(FuriThread* t) {
    assert(pthread_create(&t->native, NULL, host_thread_main, t) == 0);
}
bool furi_thread_join(FuriThread* t) {
    assert(pthread_join(t->native, NULL) == 0);
    return true;
}
void furi_thread_free(FuriThread* t) {
    pthread_mutex_destroy(&t->mutex);
    pthread_cond_destroy(&t->condition);
    free(t);
}
FuriThreadId furi_thread_get_id(FuriThread* t) {
    return t;
}
FuriThreadId furi_thread_get_current_id(void) {
    return current_thread ? current_thread : &main_thread;
}
uint32_t furi_thread_flags_set(FuriThreadId t, uint32_t flags) {
    pthread_mutex_lock(&t->mutex);
    t->flags |= flags;
    uint32_t result = t->flags;
    pthread_cond_broadcast(&t->condition);
    pthread_mutex_unlock(&t->mutex);
    return result;
}
uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout) {
    (void)options;
    FuriThread* t = furi_thread_get_current_id();
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout / 1000U;
    until.tv_nsec += (timeout % 1000U) * 1000000UL;
    if(until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&t->mutex);
    while(!(t->flags & flags) && timeout) {
        int r = timeout == FuriWaitForever ?
                    pthread_cond_wait(&t->condition, &t->mutex) :
                    pthread_cond_timedwait(&t->condition, &t->mutex, &until);
        if(r == ETIMEDOUT) break;
        assert(r == 0);
    }
    uint32_t ready = t->flags & flags;
    t->flags &= ~ready;
    pthread_mutex_unlock(&t->mutex);
    return ready;
}
#else
struct FuriThread {
    FuriThreadCallback callback;
    void* context;
    uint32_t flags;
};
static struct FuriThread main_thread;
static FuriThread* storage_thread;
static bool defer_io;
static void host_reset_threads(void) {
    main_thread.flags = 0;
    defer_io = false;
}
void host_defer_io(bool defer) {
    defer_io = defer;
    if(!defer) host_complete_io();
}
void host_complete_io(void) {
    if(!storage_thread) return;
    McStorageWorker* w = storage_thread->context;
    const unsigned state = atomic_load(&w->state);
    if(state == McIoPending) {
        mc_storage_worker_execute(w);
        atomic_store(&w->state, McIoComplete);
        main_thread.flags |= MC_WAKE_IO;
    } else if(state == McIoStopping) {
        mc_storage_worker_finish(w);
    }
}
FuriThread* furi_thread_alloc_ex(
    const char* name,
    uint32_t stack,
    FuriThreadCallback callback,
    void* context) {
    (void)name;
    assert(stack == MC_IO_STACK_SIZE);
    FuriThread* t = calloc(1, sizeof(*t));
    t->callback = callback;
    t->context = context;
    return t;
}
void furi_thread_start(FuriThread* t) {
    storage_thread = t;
}
bool furi_thread_join(FuriThread* t) {
    (void)t;
    return true;
}
void furi_thread_free(FuriThread* t) {
    if(storage_thread == t) storage_thread = NULL;
    free(t);
}
FuriThreadId furi_thread_get_id(FuriThread* t) {
    return t;
}
FuriThreadId furi_thread_get_current_id(void) {
    return &main_thread;
}
uint32_t furi_thread_flags_set(FuriThreadId t, uint32_t flags) {
    t->flags |= flags;
    if(t == storage_thread && !defer_io) host_complete_io();
    return t->flags;
}
uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout) {
    (void)options;
    if(!defer_io) host_complete_io();
    uint32_t ready = main_thread.flags & flags;
    if(ready) {
        main_thread.flags &= ~ready;
        return ready;
    }
    if(timeout == 0) return 0;
    // Trigger a stall only when the wait reaches it before the next scripted event
    if(stall_duration && (ticks >= stall_tick || timeout >= stall_tick - ticks) &&
       (input_index == input_count || script[input_index].tick > stall_tick)) {
        ticks = stall_tick + stall_duration;
        stall_duration = 0U;
        return FuriStatusErrorTimeout;
    }
    if(input_index < input_count &&
       (script[input_index].tick <= ticks || timeout == FuriWaitForever ||
        script[input_index].tick - ticks <= timeout)) {
        const HostInput* next = &script[input_index++];
        // Events delayed by a stall are delivered without moving the virtual clock backward
        if(next->tick > ticks) ticks = next->tick;
        InputEvent event = {.key = next->key, .type = next->type};
        if(viewport && next->key == InputKeyBack && next->type == InputTypeShort &&
           ((McApp*)viewport->context)->ui.screen == McScreenPlaying && host_pause_count < 4U)
            host_sim_at_pause[host_pause_count++] =
                ((McApp*)viewport->context)->ui.game.stats.play_ticks;
        assert(viewport);
        // A scripted completed gesture abbreviates its physical press/release pair.
        const bool gesture = next->type == InputTypeShort || next->type == InputTypeLong;
        const bool synthetic =
            gesture &&
            !(atomic_load(&((McApp*)viewport->context)->physical_keys) & (1U << next->key));
        if(synthetic) {
            InputEvent press = {.key = next->key, .type = InputTypePress};
            viewport->input(&press, viewport->context);
        }
        viewport->input(&event, viewport->context);
        if(synthetic) {
            InputEvent release = {.key = next->key, .type = InputTypeRelease};
            viewport->input(&release, viewport->context);
        }
        ready = main_thread.flags & flags;
        main_thread.flags &= ~ready;
        return ready;
    }
    // Fail unfinished scripts instead of hanging the host suite on an infinite wait
    assert(timeout != FuriWaitForever && "Application failed to finish script");
    ticks += timeout;
    if(ticks >= frequency * 180U && viewport) {
        const McApp* a = viewport->context;
        fprintf(
            stderr,
            "Loop timeout: screen=%u menu=%u pending=%u failed=%u input=%zu/%zu\n",
            a->ui.screen,
            a->ui.menu_index,
            a->pending_io,
            a->failed_io,
            input_index,
            input_count);
    }
    assert(ticks < frequency * 180U);
    return FuriStatusErrorTimeout;
}
#endif
// Read the virtual clock advanced by input waits rather than real wall-clock time
uint32_t furi_get_tick(void) {
    return ticks;
}
// Convert milliseconds at the configured host frequency using wide intermediate arithmetic
uint32_t furi_ms_to_ticks(uint32_t ms) {
    return (uint32_t)((uint64_t)ms * frequency / 1000U);
}
// Return the tick frequency selected for the current scripted run
uint32_t furi_kernel_get_tick_frequency(void) {
    return frequency;
}
// Return a fixed seed so setup and gameplay captures are reproducible
uint32_t furi_hal_random_get(void) {
    host_random_calls++;
    return 0x12345678U;
}
// Return the fixed host timestamp used by save metadata and Daily setup
uint32_t furi_hal_rtc_get_timestamp(void) {
    return timestamp;
}
// Return stable in-memory placeholders for storage, GUI, and notification services
void* furi_record_open(const char* name) {
    static Storage storage;
    static Gui gui;
    static NotificationApp note;
    if(strcmp(name, RECORD_STORAGE) == 0) return &storage;
    if(strcmp(name, RECORD_GUI) == 0) return &gui;
    return &note;
}
// Accept service release without freeing the static host placeholders
void furi_record_close(const char* name) {
    (void)name;
}
// Allocate the callbacks and context needed to render through the host Canvas
ViewPort* view_port_alloc(void) {
    return calloc(1U, sizeof(ViewPort));
}
// Capture final UI state for assertions before freeing the viewport
void view_port_free(ViewPort* p) {
    host_final_ui = ((McApp*)p->context)->ui;
    free(p);
    viewport = NULL;
}
// Retain the real drawing callback and its application context
void view_port_draw_callback_set(ViewPort* p, void (*fn)(Canvas*, void*), void* context) {
    p->draw = fn;
    p->context = context;
}
// Retain the input callback and its application context for host callers
void view_port_input_callback_set(ViewPort* p, void (*fn)(InputEvent*, void*), void* context) {
    p->input = fn;
    p->context = context;
}
// Count a redraw and invoke the real callback immediately on a fresh host Canvas
void view_port_update(ViewPort* p) {
    Canvas canvas = {0};
    host_redraws++;
    p->draw(&canvas, p->context);
}
// Expose the active viewport to scripted input observations
void gui_add_view_port(Gui* gui, ViewPort* p, GuiLayer layer) {
    (void)gui;
    (void)layer;
    viewport = p;
}
// Accept GUI detachment; viewport disposal releases the host allocation separately
void gui_remove_view_port(Gui* gui, ViewPort* p) {
    (void)gui;
    (void)p;
}
// Match the message fields referenced by feedback; host notification handlers do not play them
const NotificationMessage message_delay_25 = {
    .type = NotificationMessageTypeDelay,
    .data.delay.length = 25U};
const NotificationMessage message_delay_50 = {
    .type = NotificationMessageTypeDelay,
    .data.delay.length = 50U};
const NotificationMessage message_delay_100 = {
    .type = NotificationMessageTypeDelay,
    .data.delay.length = 100U};
const NotificationMessage message_delay_250 = {
    .type = NotificationMessageTypeDelay,
    .data.delay.length = 250U};
const NotificationMessage message_sound_off = {.type = NotificationMessageTypeSoundOff};
const NotificationMessage message_vibro_on = {
    .type = NotificationMessageTypeVibro,
    .data.vibro.on = true};
const NotificationMessage message_vibro_off = {.type = NotificationMessageTypeVibro};
const NotificationMessage message_red_255 = {
    .type = NotificationMessageTypeLedRed,
    .data.led.value = 255U};
const NotificationMessage message_green_255 = {
    .type = NotificationMessageTypeLedGreen,
    .data.led.value = 255U};
const NotificationMessage message_red_0 = {.type = NotificationMessageTypeLedRed};
const NotificationMessage message_green_0 = {.type = NotificationMessageTypeLedGreen};
const NotificationMessage message_blue_0 = {.type = NotificationMessageTypeLedBlue};
const NotificationMessage message_blink_stop = {.type = NotificationMessageTypeLedBlinkStop};
const NotificationMessage message_do_not_reset = {.type = NotificationMessageTypeDoNotReset};
const NotificationSequence sequence_single_vibro =
    {&message_vibro_on, &message_delay_100, &message_vibro_off, NULL};
const NotificationSequence sequence_reset_rgb =
    {&message_red_0, &message_green_0, &message_blue_0, NULL};

// Host checks render pixels and game state; physical feedback is a device concern
void notification_message(NotificationApp* app, const NotificationSequence* seq) {
    (void)app;
    (void)seq;
}
// Use the same silent host notification handler without delaying virtual time
void notification_message_block(NotificationApp* app, const NotificationSequence* seq) {
    notification_message(app, seq);
}
// Return a fixed display date so screenshots do not depend on calendar conversion
void datetime_timestamp_to_datetime(uint32_t stamp, DateTime* date) {
    (void)stamp;
    *date = (DateTime){.year = 2026U, .month = 9U, .day = 4U};
}

void (*host_render_hook)(void);
void host_render_begin(void) {
    assert(host_held_locks == 0U);
    if(host_render_hook) {
        void (*hook)(void) = host_render_hook;
        host_render_hook = NULL;
        hook();
    }
}

bool storage_file_seek(File* file, uint32_t offset, bool from_start) {
    size_t position = from_start ? offset : file->position + offset;
    if(fail_next(HostStorageSeek) || !file->data || position > file->data->size) {
        file->error = FSE_INTERNAL;
        return false;
    }
    file->position = position;
    return true;
}
void host_load_fixture(const char* local, const char* remote) {
    FILE* input = fopen(local, "rb");
    assert(input);
    HostFile* file = lookup(remote, true);
    assert(file);
    file->size = fread(file->data, 1, sizeof(file->data), input);
    assert(!ferror(input) && feof(input));
    fclose(input);
}
