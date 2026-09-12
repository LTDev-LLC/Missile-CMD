#include "storage_worker.h"
#include "help_storage.h"
#include <stdio.h>
#include <string.h>

void mc_storage_worker_execute(McStorageWorker* w) {
    McIoJob* j = &w->job;
    McPersistence* p = w->persistence;
    if(j->operation == McIoHelp) {
        mc_help_read(p->storage, &j->data.help);
        return;
    }
    p->slot = j->slot;
    p->generation = j->generation;
    p->recovered = false;
    McRunSnapshot run = {
        .game = &j->data.run.game, .stage = j->data.run.stage, .wave_start = &j->data.run.wave};
    switch(j->operation) {
    case McIoLoadSettings:
        j->result = mc_persistence_load_settings(p, &j->data.settings);
        break;
    case McIoLoadScores:
        j->result = mc_persistence_load_scores(p, &j->data.scores);
        break;
    case McIoLoadProfile:
        j->result = mc_persistence_load_profile(p, &j->data.profile);
        break;
    case McIoLoadRun:
        j->result = mc_persistence_restore(p, &run);
        j->data.run.stage = run.stage;
        break;
    case McIoSaveRun:
        j->result = mc_persistence_checkpoint(p, &run);
        break;
    case McIoDeleteRun:
        j->result = mc_persistence_remove_checkpoint(p);
        break;
    case McIoLoadPace:
    case McIoSavePace:
        j->result = mc_persistence_pace(
            p, &j->data.pace.game, &j->data.pace.pace, j->operation == McIoSavePace);
        break;
    case McIoSlots:
        mc_persistence_slot_info(p, j->slot, &j->data.info);
        j->result = j->data.info.status;
        break;
    case McIoSaveSettings:
        j->result = mc_persistence_save_settings(p, &j->data.settings);
        break;
    case McIoSaveScores:
        j->result = mc_persistence_save_scores(p, &j->data.scores);
        break;
    case McIoSaveProfile:
        j->result = mc_persistence_save_profile(p, &j->data.profile);
        break;
    case McIoHistory:
        if(j->data.cleanup.action && !mc_persistence_history_clear(p)) {
            j->result = McStorageIoError;
            break;
        }
        j->result = mc_persistence_history_step(p);
        break;
    case McIoCleanupInit:
    case McIoCleanupStep: {
        McCleanup* c = w->cleanup;
        if(c->state == McCleanupValidating && j->operation == McIoCleanupStep &&
           !j->data.cleanup.action) {
            // Borrow the job union for codec outputs; do not allocate a second game on the stack.
            const McCleanupReply reply = j->data.cleanup;
            McStorageResult result;
            if(c->validation == 0U)
                result = mc_persistence_load_settings(p, &j->data.settings);
            else if(c->validation == 1U)
                result = mc_persistence_load_scores(p, &j->data.scores);
            else if(c->validation == 2U)
                result = mc_persistence_load_profile(p, &j->data.profile);
            else {
                p->slot = c->validation - 3U;
                result = mc_persistence_restore(p, &run);
            }
            j->data.cleanup = reply;
            mc_cleanup_validated(c, result);
        }
        McCleanupReply* r = &j->data.cleanup;
        r->requested_index = r->index;
        r->requested_offset = r->offset;
        if(j->operation == McIoCleanupInit) mc_cleanup_init(c, p->storage);
        if(r->action == 1U) mc_cleanup_deinit(c);
        if(r->action == 2U) mc_cleanup_approve(c);
        if(r->action == 3U) mc_cleanup_retry(c);
        if(r->action == 4U) mc_cleanup_migrate(c);
        if(r->action != 1U) mc_cleanup_step(c);
        if(r->action != 1U) mc_cleanup_migration_step(c, p->scratch);
        r->state = r->action == 1U ? McCleanupDone : c->state;
        r->count = c->count;
        r->remaining = c->remaining;
        r->approved = c->approved;
        r->limited = c->limited;
        r->can_migrate = c->source != NULL;
        r->migrating = c->migrating;
        snprintf(r->source, sizeof(r->source), "%.40s", c->source ? c->source->name : "");
        if(r->count)
            r->index %= r->count;
        else
            r->index = 0;
        const char* name = mc_cleanup_name(c, r->index);
        r->length = strlen(name);
        if(r->offset >= r->length) r->offset = r->length ? ((r->length - 1U) / 40U) * 40U : 0U;
        snprintf(r->name, sizeof(r->name), "%.40s", name + r->offset);
        j->result = r->state == McCleanupFailed ? c->result : McStorageOk;
        if(r->state == McCleanupDone) mc_cleanup_deinit(c);
        break;
    }
    }
    j->generation = p->generation;
    j->timestamp = p->timestamp;
    j->recovered = p->recovered;
    j->history_pending = mc_persistence_history_pending(p);
    j->history_required = mc_persistence_history_required(p);
}

void mc_storage_worker_finish(McStorageWorker* w) {
    mc_cleanup_deinit(w->cleanup);
    mc_persistence_deinit(w->persistence);
    atomic_store_explicit(&w->state, McIoStopped, memory_order_release);
    furi_thread_flags_set(w->owner, MC_WAKE_IO);
}

static int32_t mc_storage_thread(void* context) {
    McStorageWorker* w = context;
    for(;;) {
        furi_thread_flags_wait(1U, FuriFlagWaitAny, FuriWaitForever);
        const unsigned state = atomic_load_explicit(&w->state, memory_order_acquire);
        if(state == McIoStopping) break;
        if(state != McIoPending) continue;
        mc_storage_worker_execute(w);
        atomic_store_explicit(&w->state, McIoComplete, memory_order_release);
        furi_thread_flags_set(w->owner, MC_WAKE_IO);
    }
    mc_storage_worker_finish(w);
    return 0;
}
void mc_storage_worker_init(McStorageWorker* w, McPersistence* p, McCleanup* c) {
    memset(w, 0, sizeof(*w));
    w->persistence = p;
    w->cleanup = c;
    w->owner = furi_thread_get_current_id();
    atomic_init(&w->state, McIoIdle);
    w->thread = furi_thread_alloc_ex("Missile storage", MC_IO_STACK_SIZE, mc_storage_thread, w);
    furi_check(w->thread);
    furi_thread_start(w->thread);
}
void mc_storage_worker_submit(McStorageWorker* w) {
    atomic_store_explicit(&w->state, McIoPending, memory_order_release);
    furi_thread_flags_set(furi_thread_get_id(w->thread), 1U);
}
void mc_storage_worker_stop(McStorageWorker* w) {
    atomic_store_explicit(&w->state, McIoStopping, memory_order_release);
    furi_thread_flags_set(furi_thread_get_id(w->thread), 1U);
}
void mc_storage_worker_free(McStorageWorker* w) {
    furi_check(atomic_load_explicit(&w->state, memory_order_acquire) == McIoStopped);
    furi_thread_join(w->thread);
    furi_thread_free(w->thread);
}
