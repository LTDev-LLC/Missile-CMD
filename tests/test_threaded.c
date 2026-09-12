// Exercise the real worker on a native thread while rendering and input remain available.
#include "host_platform.h"
#include "app_internal.h"
#include "render.h"
#include <assert.h>
#include <stdio.h>
#include "../src/app.c"

static void settle(McApp* app) {
    for(unsigned i = 0; i < 128; i++) {
        mc_app_process_io(app, 0);
        const unsigned state = atomic_load(&app->worker.state);
        if(state == McIoIdle && !app->startup_step && app->ui.screen == McScreenTitle &&
           !app->pending_io)
            return;
        if(state == McIoPending) furi_thread_flags_wait(MC_WAKE_IO, FuriFlagWaitAny, 3000);
    }
    assert(false && "worker failed to settle");
}
static void test_delayed_help(void) {
    host_reset();
    McApp* app = mc_app_alloc();
    settle(app);
    host_load_fixture(MC_HELP_ASSET_PATH, MC_HELP_PATH);
    app->ui.screen = McScreenHudGuide;
    host_defer_io(true);
    mc_app_process_io(app, 0);
    host_wait_storage_blocked();
    assert(app->worker.job.operation == McIoHelp);
    Canvas canvas = {0};
    mc_view_draw_callback(&canvas, app);
    InputEvent key = {.key = InputKeyRight, .type = InputTypePress}, received;
    mc_view_input_callback(&key, app);
    assert(mc_app_get_input(app, &received, 0) == FuriStatusOk);
    mc_app_handle_input(app, &received);
    assert(app->ui.guide_page == 1);
    mc_app_process_io(app, 0);
    assert(app->ui.help.id == McHelpHud1 && app->ui.help.status == McHelpLoading);
    mc_app_request_exit(app);
    mc_app_process_exit(app, 4000);
    assert(app->running && atomic_load(&app->worker.state) == McIoPending);
    host_complete_io();
    while(atomic_load(&app->worker.state) != McIoComplete)
        assert(furi_thread_flags_wait(MC_WAKE_IO, FuriFlagWaitAny, 3000));
    mc_app_process_io(app, 4000);
    assert(app->ui.help.id == McHelpNoPage && app->ui.help.status == McHelpEmpty);
    mc_app_free(app);
}

int main(void) {
    test_delayed_help();
    host_reset();
    McApp* app = mc_app_alloc();
    settle(app);
    app->ui.settings.sound = false;
    mc_app_settings_changed(app);
    mc_app_flush_settings(app);
    host_defer_io(true);
    assert(mc_app_process_io(app, 0));
    host_wait_storage_blocked();
    assert(atomic_load(&app->worker.state) == McIoPending);
    // Changing settings cannot change the immutable request currently being written.
    app->ui.settings.sound = true;
    mc_app_settings_changed(app);
    Canvas canvas = {0};
    mc_view_draw_callback(&canvas, app);
    InputEvent press = {.key = InputKeyDown, .type = InputTypePress}, received;
    mc_view_input_callback(&press, app);
    assert(mc_app_get_input(app, &received, 0) == FuriStatusOk);
    assert(received.key == InputKeyDown && received.type == InputTypePress);
    mc_app_request_exit(app);
    mc_app_process_exit(app, 4000);
    assert(app->running && app->ui.exit_slow && atomic_load(&app->worker.state) == McIoPending);
    // Explicitly discarding queued changes still cannot free an active worker.
    app->ui.menu_index = 2;
    InputEvent ok = {.key = InputKeyOk, .type = InputTypeShort};
    InputEvent back = {.key = InputKeyBack, .type = InputTypeShort};
    mc_app_handle_input(app, &ok);
    assert(app->ui.screen == McScreenConfirm && !app->ui.exit_discard);
    mc_app_handle_input(app, &back);
    assert(app->ui.screen == McScreenExiting && !app->ui.exit_discard);
    app->ui.menu_index = 2;
    mc_app_handle_input(app, &ok);
    mc_app_handle_input(app, &ok);
    assert(app->ui.exit_discard && !app->pending_io && !app->settings_dirty);
    mc_app_handle_input(app, &back);
    assert(app->ui.exit_pending); // Cannot return with writes permanently discarded.
    mc_app_process_exit(app, 4000);
    assert(atomic_load(&app->worker.state) == McIoPending);
    host_complete_io();
    while(atomic_load(&app->worker.state) != McIoComplete)
        assert(furi_thread_flags_wait(MC_WAKE_IO, FuriFlagWaitAny, 3000));
    mc_app_process_io(app, 4000);
    mc_app_process_exit(app, 4000);
    while(atomic_load(&app->worker.state) != McIoStopped)
        assert(furi_thread_flags_wait(MC_WAKE_IO, FuriFlagWaitAny, 3000));
    mc_app_process_exit(app, 4000);
    assert(!app->running);
    mc_app_free(app);
    puts("threaded storage, input, rendering, and teardown tests passed");
    return 0;
}
