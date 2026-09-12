// Exercise version selection, real persisted data, and interrupted migration on virtual storage.
#include "host_platform.h"
#include "app_internal.h"
#include "help_storage.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/app.c"

#define OLD_DATA APP_DATA_PATH("0.9.0+build.2")
#define NEW_DATA APP_DATA_PATH(MC_DATA_FOLDER)

static void write_bytes(Storage* storage, const char* path, const void* bytes, size_t size) {
    File* file = storage_file_alloc(storage);
    assert(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS));
    assert(storage_file_write(file, bytes, size) == size);
    assert(storage_file_sync(file));
    storage_file_close(file);
    storage_file_free(file);
}

static void check_bytes(Storage* storage, const char* path, const void* bytes, size_t size) {
    uint8_t buffer[2048];
    assert(size <= sizeof(buffer));
    File* file = storage_file_alloc(storage);
    assert(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING));
    assert(storage_file_size(file) == size);
    assert(storage_file_read(file, buffer, size) == size);
    assert(!memcmp(buffer, bytes, size));
    storage_file_close(file);
    storage_file_free(file);
}

static void send(McApp* app, InputKey key) {
    const InputEvent event = {
        .key = key, .type = key == InputKeyRight ? InputTypePress : InputTypeShort};
    mc_app_handle_input(app, &event);
}

static void settle(McApp* app) {
    for(unsigned step = 0; step < 4096; step++) {
        mc_app_process_io(app, 0);
        assert(!app->cleanup.allocated || !app->persistence.history);
        if(!app->startup_step && app->ui.screen == McScreenTitle) return;
        if(atomic_load(&app->worker.state) == McIoIdle && app->ui.screen == McScreenCleanup &&
           (app->ui.cleanup_state == McCleanupPrompt || app->ui.cleanup_state == McCleanupFailed))
            return;
    }
    assert(!"Migration did not settle");
}

static McApp* prompt(void) {
    McApp* app = mc_app_alloc();
    settle(app);
    assert(app->ui.screen == McScreenCleanup && app->ui.cleanup_state == McCleanupPrompt);
    assert(app->ui.cleanup_can_migrate);
    assert(!strcmp(app->ui.cleanup_source, "0.9.0+build.2"));
    return app;
}

static void migrate(McApp* app) {
    send(app, InputKeyRight);
    assert(app->ui.menu_index == 1U);
    send(app, InputKeyOk);
    assert(app->cleanup_action == 4U);
}

static uint8_t payload[1300];
static uint32_t run_hashes[MC_SAVE_SLOTS];

static Storage* fixture(bool current_settings) {
    host_reset();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    const char* folders[] = {
        NEW_DATA,
        OLD_DATA,
        OLD_DATA "/nested",
        OLD_DATA "/nested/deeper",
        NEW_DATA "/nested",
        APP_DATA_PATH("0.8.0"),
        APP_DATA_PATH("0.9.0+build.1"),
        APP_DATA_PATH("0.99.0"),
        APP_DATA_PATH("2.0.0"),
        APP_DATA_PATH("v7"),
        APP_DATA_PATH("notes")};
    for(unsigned i = 0; i < sizeof(folders) / sizeof(folders[0]); i++)
        assert(storage_common_mkdir(storage, folders[i]) == FSE_OK);
    for(unsigned i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 17U);
    write_bytes(storage, OLD_DATA "/nested/deeper/custom.bin", payload, sizeof(payload));
    write_bytes(storage, OLD_DATA "/nested/empty", "", 0);
    write_bytes(storage, OLD_DATA "/nested/conflict", "old", 3);
    write_bytes(storage, NEW_DATA "/nested/conflict", "current", 7);
    write_bytes(storage, OLD_DATA "/after-nested", "after", 5);
    write_bytes(storage, APP_DATA_PATH("0.8.0/older"), "older", 5);
    write_bytes(storage, APP_DATA_PATH("0.9.0+build.1/older"), "older", 5);
    write_bytes(storage, APP_DATA_PATH("v7/older"), "older", 5);
    write_bytes(storage, APP_DATA_PATH("2.0.0/newer"), "newer", 5);
    write_bytes(storage, APP_DATA_PATH("notes/keep"), "notes", 5);
    write_bytes(storage, OLD_DATA "/help.bin", "old-help", 8);
    write_bytes(storage, APP_DATA_PATH("0.99.0/help.bin"), "only-help", 9);
    host_load_fixture(MC_HELP_ASSET_PATH, MC_HELP_PATH);

    McPersistence p;
    assert(mc_persistence_init(&p, storage));
    McSettings settings;
    mc_settings_defaults(&settings);
    settings.cursor_speed = McCursorFast;
    settings.resume_countdown = false;
    assert(mc_persistence_save_settings(&p, &settings) == McStorageOk);
    assert(mc_persistence_save_settings(&p, &settings) == McStorageOk);
    McScoreTables scores;
    mc_score_tables_defaults(&scores);
    McScoreEntry entry = {.score = 1234, .wave = 5, .difficulty = McDifficultyCommand};
    assert(mc_score_tables_insert(&scores, &entry));
    assert(mc_persistence_save_scores(&p, &scores) == McStorageOk);
    McProfile profile;
    mc_profile_defaults(&profile);
    profile.best_score = 1234;
    assert(mc_persistence_save_profile(&p, &profile) == McStorageOk);
    for(unsigned slot = 0; slot < MC_SAVE_SLOTS; slot++) {
        McGame game;
        McWaveStart wave;
        mc_game_start_run(&game, McDifficultyCommand, 42U + slot);
        assert(mc_wave_start_capture(&wave, &game));
        for(unsigned tick = 0; tick < 20; tick++)
            mc_game_step(&game, NULL);
        run_hashes[slot] = mc_game_state_hash(&game);
        p.slot = slot;
        McRunSnapshot run = {.game = &game, .stage = McRunStageActive, .wave_start = &wave};
        assert(mc_persistence_checkpoint(&p, &run) == McStorageOk);
    }
    const char* names[] = {
        "settings.dat",
        "settings.bak",
        "scores.dat",
        "profile.dat",
        "save0.dat",
        "save1.dat",
        "save2.dat"};
    for(unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char from[128], to[128];
        snprintf(from, sizeof(from), NEW_DATA "/%s", names[i]);
        snprintf(to, sizeof(to), OLD_DATA "/%s", names[i]);
        assert(storage_common_rename(storage, from, to) == FSE_OK);
    }
    host_corrupt(OLD_DATA "/settings.dat"); // A usable backup must migrate along with the primary.
    if(current_settings) {
        p.trusted_primary = 0;
        settings.cursor_speed = McCursorSlow;
        assert(mc_persistence_save_settings(&p, &settings) == McStorageOk);
    }
    mc_persistence_deinit(&p);
    return storage;
}

static void check_migrated(McApp* app, Storage* storage, bool current_settings) {
    assert(app->ui.screen == McScreenTitle && !app->cleanup.allocated);
    assert(app->ui.settings.cursor_speed == (current_settings ? McCursorSlow : McCursorFast));
    assert(!app->ui.settings.resume_countdown);
    assert(app->ui.profile.best_score == 1234U);
    assert(app->ui.scores.boards[McDifficultyCommand].entries[0].score == 1234U);
    assert(app->ui.has_suspended_run);
    for(unsigned slot = 0; slot < MC_SAVE_SLOTS; slot++) {
        McGame game;
        McWaveStart wave;
        McRunSnapshot run = {.game = &game, .wave_start = &wave};
        app->persistence.slot = slot;
        assert(mc_persistence_restore(&app->persistence, &run) == McStorageOk);
        assert(mc_game_state_hash(&game) == run_hashes[slot] && wave.size);
    }
    check_bytes(storage, NEW_DATA "/nested/deeper/custom.bin", payload, sizeof(payload));
    check_bytes(storage, NEW_DATA "/nested/empty", "", 0);
    check_bytes(storage, NEW_DATA "/nested/conflict", "current", 7);
    check_bytes(storage, NEW_DATA "/after-nested", "after", 5);
    check_bytes(storage, APP_DATA_PATH("2.0.0/newer"), "newer", 5);
    check_bytes(storage, APP_DATA_PATH("notes/keep"), "notes", 5);
    assert(storage_common_stat(storage, OLD_DATA, NULL) == FSE_NOT_EXIST);
    assert(storage_common_stat(storage, APP_DATA_PATH("0.8.0"), NULL) == FSE_NOT_EXIST);
    assert(storage_common_stat(storage, APP_DATA_PATH("v7"), NULL) == FSE_NOT_EXIST);
    assert(storage_common_stat(storage, APP_DATA_PATH("0.99.0"), NULL) == FSE_NOT_EXIST);
    assert(!storage_file_exists(storage, NEW_DATA "/.migration.tmp"));
    McHelpPage help = {.id = 0};
    mc_help_read(storage, &help);
    assert(help.status == McHelpReady);
}

static void test_selection(void) {
    const char* versions[] = {
        "v2",
        "v10",
        "0.9.0",
        "1.0.0-alpha",
        "1.0.0-alpha.1",
        "1.0.0-alpha.beta",
        "1.0.0-beta",
        "1.0.0-beta.2",
        "1.0.0-beta.11",
        "1.0.0-rc.1",
        "1.0.0",
        "1.0.10",
        "1.1.0",
        "10000000000000000000000000.0.0"};
    for(unsigned i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        assert(!mc_cleanup_version_compare(versions[i], versions[i]));
        for(unsigned j = 0; j < i; j++) {
            assert(mc_cleanup_version_compare(versions[i], versions[j]) > 0);
            assert(mc_cleanup_version_compare(versions[j], versions[i]) < 0);
        }
    }
    assert(!mc_cleanup_version_compare("1.0.0+one", "1.0.0+two"));
    assert(!mc_cleanup_version_compare("1.0.0-rc.1+one", "1.0.0-rc.1+two"));
    assert(mc_cleanup_version_compare("1.0.0-a-b", "1.0.0-a-a") > 0);
    assert(!mc_cleanup_version_compare("v0002", "v2"));
}

static void test_migrate_and_conflicts(void) {
    for(unsigned current = 0; current < 2; current++) {
        Storage* storage = fixture(current);
        McApp* app = prompt();
        migrate(app);
        settle(app);
        check_migrated(app, storage, current);
        mc_app_free(app);
    }
}

static void test_failures_and_retry(void) {
    // Fail every mutation in turn: mkdir, staging, writes, sync, publish, and pruning.
    for(int fault = 0; fault < 256; fault++) {
        Storage* storage = fixture(false);
        McApp* app = prompt();
        host_fail_storage_after(fault);
        migrate(app);
        settle(app);
        const bool failed = app->ui.cleanup_state == McCleanupFailed;
        if(failed) {
            if(!app->cleanup.migrated) {
                check_bytes(
                    storage, OLD_DATA "/nested/deeper/custom.bin", payload, sizeof(payload));
                assert(storage_file_exists(storage, APP_DATA_PATH("0.8.0/older")));
            }
            host_fail_storage_after(-1);
            send(app, InputKeyOk); // Retry remains selected after a failed migration.
            settle(app);
        }
        host_fail_storage_after(-1);
        check_migrated(app, storage, false);
        mc_app_free(app);
        if(!failed) return;
    }
    assert(!"Mutation fault sweep never reached a successful migration");
}

static void test_read_failures(void) {
    const HostStorageOperation faults[] = {
        HostStorageOpen,
        HostStorageRead,
        HostStorageSeek,
        HostStorageStat,
        HostStorageDirOpen,
        HostStorageDirRead};
    for(unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        Storage* storage = fixture(false);
        McApp* app = prompt();
        host_fail_next(faults[i]);
        migrate(app);
        settle(app);
        assert(app->ui.cleanup_state == McCleanupFailed && !app->cleanup.migrated);
        check_bytes(storage, OLD_DATA "/nested/deeper/custom.bin", payload, sizeof(payload));
        assert(storage_file_exists(storage, APP_DATA_PATH("0.8.0/older")));
        send(app, InputKeyOk);
        settle(app);
        check_migrated(app, storage, false);
        mc_app_free(app);
    }
}

static void test_restart_after_publication(void) {
    for(unsigned pruning = 0; pruning < 2; pruning++) {
        Storage* storage = fixture(false);
        McApp* app = prompt();
        migrate(app);
        bool interrupted = false;
        for(unsigned step = 0; step < 4096; step++) {
            mc_app_process_io(app, 0);
            if(app->cleanup.migrated &&
               (!pruning || !storage_file_exists(storage, APP_DATA_PATH("0.8.0/older")))) {
                interrupted = true;
                break;
            }
        }
        assert(interrupted);
        mc_app_free(app);
        app = prompt();
        migrate(app);
        settle(app);
        check_migrated(app, storage, false);
        mc_app_free(app);
    }
}

static void test_interruption_and_validation(void) {
    for(unsigned restart = 0; restart < 2; restart++) {
        Storage* storage = fixture(false);
        McApp* app = prompt();
        migrate(app);
        for(unsigned step = 0; step < 4096 && !app->cleanup.verifying; step++)
            mc_app_process_io(app, 0);
        assert(app->cleanup.verifying);
        if(restart) {
            mc_app_free(app);
            app = prompt();
            migrate(app);
        } else {
            host_corrupt(NEW_DATA "/.migration.tmp");
            settle(app);
            assert(app->ui.cleanup_state == McCleanupFailed && !app->cleanup.migrated);
            check_bytes(storage, OLD_DATA "/nested/deeper/custom.bin", payload, sizeof(payload));
            send(app, InputKeyOk);
        }
        settle(app);
        check_migrated(app, storage, false);
        mc_app_free(app);
    }
    Storage* storage = fixture(false);
    host_corrupt(OLD_DATA "/settings.bak");
    McApp* app = prompt();
    migrate(app);
    settle(app);
    assert(app->ui.cleanup_state == McCleanupFailed && !app->cleanup.migrated);
    assert(storage_file_exists(storage, OLD_DATA "/settings.bak"));
    assert(storage_file_exists(storage, APP_DATA_PATH("0.8.0/older")));
    send(app, InputKeyBack);
    settle(app);
    assert(app->ui.screen == McScreenTitle);
    assert(storage_file_exists(storage, OLD_DATA "/settings.bak"));
    mc_app_free(app);
}

int main(void) {
    test_selection();
    test_migrate_and_conflicts();
    test_failures_and_retry();
    test_read_failures();
    test_restart_after_publication();
    test_interruption_and_validation();
    puts("migration selection, files, saves, conflicts, interruptions, and retry tests passed");
    return 0;
}
