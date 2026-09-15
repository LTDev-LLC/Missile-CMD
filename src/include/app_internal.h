#pragma once

#include "app_model.h"
#include "feedback.h"
#include "persistence.h"
#include "runtime.h"
#include "storage_worker.h"

#include <furi.h>
#include <stdatomic.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#define MC_HELD_LEFT  (1U << 0)
#define MC_HELD_RIGHT (1U << 1)
#define MC_HELD_UP    (1U << 2)
#define MC_HELD_DOWN  (1U << 3)

// File-family bits stay queued until success or explicit cancellation
typedef uint16_t McPendingIo;
enum {
    McPendingIoNone = 0U,
    McPendingIoSettings = 1U << 0,
    McPendingIoScores = 1U << 1,
    McPendingIoProfile = 1U << 2,
    McPendingIoRunSave = 1U << 3,
    McPendingIoRunDelete = 1U << 4,
    McPendingIoRunLoad = 1U << 5,
    McPendingIoSlots = 1U << 6,
    McPendingIoPaceLoad = 1U << 7,
    McPendingIoPaceSave = 1U << 8,
    McPendingIoHistory = 1U << 9,
};

typedef struct {
    McUiModel ui;
    McFeedback feedback;
    McRenderSnapshot* render_snapshot;
    McPersistence persistence;
    McStorageWorker worker;
    uint32_t run_revision, run_identity, exit_started;
    uint32_t settings_revision, scores_revision, profile_revision;
    bool history_pending, history_required, clear_history, io_recovered;
    bool cleanup_refresh, cleanup_requested, cleanup_management;
    McCleanup cleanup;
    // Deferred cleanup request: 0 none, 1 keep, 2 purge, 3 retry, 4 import
    uint8_t cleanup_action;
    McPace pace;
    McWaveStart wave_start;
    McSettings saved_settings;
    Gui* gui;
    NotificationApp* notification;
    Storage* storage;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    atomic_uint physical_keys;
    atomic_uint input_overflowed;
    atomic_uint admitted_keys;
    FuriMutex* mutex;
    // Deadlines use platform ticks and signed differences for tick-counter wrap
    uint32_t settings_due_tick;
    uint32_t retry_due_tick;
    uint32_t notice_expiry_tick;
    McRenderClock render_clock;
    uint8_t startup_step, slot_scan;
    McGameChange pending_changes;
    McPendingIo pending_io;
    McPendingIo failed_io;
    McRunStage pending_run_stage;
    uint16_t cursor_hold_ticks;
    uint8_t held_directions;
    int8_t cursor_direction_x;
    int8_t cursor_direction_y;
    bool settings_dirty;
    bool scores_dirty;
    bool profile_dirty;
    bool run_dirty;
    bool save_title_on_success;
    bool running;
    bool input_batch;
} McApp;

// The worker mailbox is inline in McApp. Reserve 1 KiB for the 32-event queue,
// worker/thread metadata, mutex and viewport. Startup cleanup releases its list
// before gameplay or history maintenance can allocate the smaller pace history.
// The application and worker stacks each have a separate 4 KiB budget.
_Static_assert(
    MC_HISTORY_ALLOCATION_BUDGET <= MC_CLEANUP_ALLOCATION_BUDGET,
    "history must fit the heap allowance shared with startup cleanup");
_Static_assert(
    sizeof(McApp) + sizeof(McRenderSnapshot) + MC_CLEANUP_ALLOCATION_BUDGET +
            MC_PERSISTENCE_SCRATCH_SIZE + 1024U <=
        12U * 1024U,
    "application state and cleanup allocations exceed 12 KiB budget");

void mc_app_handle_input(McApp* app, const InputEvent* event);
void mc_app_start_new_run(McApp* app, bool allow_control_card);
void mc_app_open_run_setup(McApp* app);
void mc_app_resume_run(McApp* app);
void mc_app_open_settings(McApp* app, McScreen return_screen);
void mc_app_open_scores(McApp* app, McScreen return_screen);
void mc_app_confirm(McApp* app, McConfirmAction action, McScreen return_screen);
void mc_app_change_difficulty(McApp* app, bool increment);
void mc_app_refresh_high_score(McApp* app);
void mc_app_queue_run_save(McApp* app, McRunStage stage, bool return_to_title);
void mc_app_queue_run_delete(McApp* app);
void mc_app_settings_changed(McApp* app);
void mc_app_flush_settings(McApp* app);
void mc_app_save_scores(McApp* app);
void mc_app_dispatch_events(McApp* app, const McGameEventBuffer* events);
void mc_app_direction_changed(McApp* app, bool guarantee_step);
void mc_app_refresh_render_cache(McApp* app);

void mc_app_retry(McApp* app, bool random_seed);
void mc_app_play_last(McApp* app);
void mc_app_practice_wave(McApp* app);
void mc_app_continue(McApp* app);
void mc_app_retry_storage(McApp* app);
void mc_app_select_slot(McApp* app, uint8_t slot);
void mc_app_clear_pace(McApp* app);

void mc_app_cleanup_select(McApp* app, bool next);
void mc_app_open_version_data(McApp* app);

void mc_app_capture_wave(McApp* app);

void mc_app_request_exit(McApp* app);

void mc_app_profile_changed(McApp* app);

void mc_app_clear_directions(McApp* app);
