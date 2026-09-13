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
        .key = key, .type = key < InputKeyOk ? InputTypePress : InputTypeShort};
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
        APP_DATA_PATH(MC_DATA_FOLDER "+build.other"),
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
    write_bytes(storage, APP_DATA_PATH(MC_DATA_FOLDER "+build.other/equal"), "equal", 5);
    write_bytes(storage, APP_DATA_PATH("notes/keep"), "notes", 5);
    write_bytes(storage, OLD_DATA "/help.bin", "old-help", 8);
    write_bytes(storage, OLD_DATA "/" MC_VERSION_REVIEW_FILE, "MCV1", 4);
    write_bytes(storage, OLD_DATA "/" MC_VERSION_REVIEW_FILE ".tmp", "old-temp", 8);
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
    assert(storage_common_stat(storage, OLD_DATA, NULL) == FSE_OK);
    assert(storage_common_stat(storage, APP_DATA_PATH("0.8.0"), NULL) == FSE_OK);
    assert(storage_common_stat(storage, APP_DATA_PATH("v7"), NULL) == FSE_OK);
    assert(storage_common_stat(storage, APP_DATA_PATH("0.99.0"), NULL) == FSE_OK);
    check_bytes(storage, OLD_DATA "/nested/deeper/custom.bin", payload, sizeof(payload));
    check_bytes(storage, APP_DATA_PATH(MC_DATA_FOLDER "+build.other/equal"), "equal", 5);
    assert(mc_cleanup_reviewed(storage));
    assert(!storage_file_exists(storage, NEW_DATA "/" MC_VERSION_REVIEW_FILE ".tmp"));
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

static size_t legacy_settings(const McSettings* settings, uint8_t* bytes) {
    assert(mc_settings_encode(settings, bytes, MC_SETTINGS_ENCODED_SIZE));
    const size_t size = MC_SETTINGS_ENCODED_SIZE - 1U;
    mc_write_u16(bytes + 4, 2U);
    mc_write_u16(bytes + 6, 29U);
    mc_write_u32(bytes + size - 4U, mc_crc32(bytes, size - 4U));
    return size;
}

static void test_schema_upgrade(void) {
    Storage* storage = fixture(false);
    McSettings expected;
    mc_settings_defaults(&expected);
    expected.cursor_speed = McCursorFast;
    expected.resume_countdown = false;
    expected.sound = false;
    expected.tap_pixels = 3;
    uint8_t legacy[MC_SETTINGS_ENCODED_SIZE], current[MC_SETTINGS_ENCODED_SIZE];
    const size_t size = legacy_settings(&expected, legacy);
    write_bytes(storage, OLD_DATA "/settings.bak", legacy, size);
    McApp* app = prompt();
    migrate(app);
    settle(app);
    check_migrated(app, storage, false);
    assert(!app->ui.settings.sound && app->ui.settings.tap_pixels == 3);
    assert(app->ui.settings.invert_colors == expected.invert_colors);
    assert(mc_settings_encode(&expected, current, sizeof(current)) == sizeof(current));
    check_bytes(storage, NEW_DATA "/settings.dat", current, sizeof(current));
    check_bytes(storage, OLD_DATA "/settings.bak", legacy, size);
    mc_app_free(app);
}

static void test_missing_configuration(void) {
    host_reset();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    assert(storage_common_mkdir(storage, OLD_DATA) == FSE_OK);
    write_bytes(storage, OLD_DATA "/custom.bin", "custom", 6);
    McApp* app = prompt();
    migrate(app);
    settle(app);
    assert(app->ui.screen == McScreenTitle && mc_cleanup_reviewed(storage));
    uint8_t bytes[MC_PERSISTENCE_SCRATCH_SIZE];
    McSettings settings;
    mc_settings_defaults(&settings);
    size_t size = mc_settings_encode(&settings, bytes, sizeof(bytes));
    check_bytes(storage, NEW_DATA "/settings.dat", bytes, size);
    McScoreTables scores;
    mc_score_tables_defaults(&scores);
    size = mc_score_tables_encode(&scores, bytes, sizeof(bytes));
    check_bytes(storage, NEW_DATA "/scores.dat", bytes, size);
    McProfile profile;
    mc_profile_defaults(&profile);
    size = mc_profile_encode(&profile, bytes, sizeof(bytes));
    check_bytes(storage, NEW_DATA "/profile.dat", bytes, size);
    for(unsigned slot = 0; slot < MC_SAVE_SLOTS; slot++) {
        McSaveInfo info;
        mc_persistence_slot_info(&app->persistence, slot, &info);
        assert(info.status == McStorageMissing);
    }
    check_bytes(storage, NEW_DATA "/custom.bin", "custom", 6);
    check_bytes(storage, OLD_DATA "/custom.bin", "custom", 6);
    mc_app_free(app);
}

static void test_upgrade_failures(void) {
    const HostStorageOperation operations[] = {
        HostStorageOpen,
        HostStorageRead,
        HostStorageWrite,
        HostStorageSync,
        HostStorageRename,
        HostStorageStat};
    // Exercise every read, write, sync and publication point with old data, a current
    // primary, or only a current backup. Restart persistence before retrying each fault.
    for(unsigned current_copy = 0; current_copy < 3; current_copy++) {
        for(unsigned op = 0; op < sizeof(operations) / sizeof(operations[0]); op++) {
            bool finished = false;
            for(unsigned fault = 0; fault < 128 && !finished; fault++) {
                host_reset();
                Storage* storage = furi_record_open(RECORD_STORAGE);
                assert(storage_common_mkdir(storage, OLD_DATA) == FSE_OK);
                assert(storage_common_mkdir(storage, NEW_DATA) == FSE_OK);
                McSettings expected, loaded;
                mc_settings_defaults(&expected);
                expected.cursor_speed = McCursorFast;
                uint8_t old[MC_SETTINGS_ENCODED_SIZE], prior[MC_SETTINGS_ENCODED_SIZE];
                size_t size = legacy_settings(&expected, old);
                write_bytes(storage, OLD_DATA "/settings.dat", old, size);
                const char* prior_path = current_copy == 1 ? NEW_DATA "/settings.dat" :
                                                             NEW_DATA "/settings.bak";
                if(current_copy) {
                    expected.cursor_speed = McCursorSlow;
                    legacy_settings(&expected, prior);
                    write_bytes(storage, prior_path, prior, size);
                }
                McPersistence p;
                assert(mc_persistence_init(&p, storage));
                memset(host_storage_calls, 0, sizeof(host_storage_calls));
                host_fail_operation_after(operations[op], fault);
                McStorageResult result =
                    mc_persistence_migrate_settings(&p, "0.9.0+build.2", &loaded);
                finished = host_storage_calls[operations[op]] <= fault;
                host_fail_next(HostStorageOperationCount); // Clear an unused injected fault.
                check_bytes(storage, OLD_DATA "/settings.dat", old, size);
                if(result != McStorageOk) {
                    assert(result == McStorageIoError || result == McStorageInvalid);
                    if(current_copy)
                        check_bytes(
                            storage,
                            storage_file_exists(storage, prior_path) ? prior_path :
                                                                       NEW_DATA "/settings.bak",
                            prior,
                            size);
                    else
                        assert(!storage_file_exists(storage, NEW_DATA "/settings.dat"));
                }
                mc_persistence_deinit(&p);
                assert(mc_persistence_init(&p, storage));
                assert(
                    mc_persistence_migrate_settings(&p, "0.9.0+build.2", &loaded) == McStorageOk);
                assert(loaded.cursor_speed == expected.cursor_speed);
                size = mc_settings_encode(&expected, prior, sizeof(prior));
                check_bytes(storage, NEW_DATA "/settings.dat", prior, size);
                mc_persistence_deinit(&p);
            }
            assert(finished);
        }
    }
}

static void test_unsupported_settings(void) {
    host_reset();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    assert(storage_common_mkdir(storage, OLD_DATA) == FSE_OK);
    McSettings settings;
    mc_settings_defaults(&settings);
    uint8_t bytes[MC_SETTINGS_ENCODED_SIZE];
    assert(mc_settings_encode(&settings, bytes, sizeof(bytes)));
    mc_write_u16(bytes + 4, 99U); // Valid checksum, unsupported schema.
    mc_write_u32(bytes + sizeof(bytes) - 4U, mc_crc32(bytes, sizeof(bytes) - 4U));
    write_bytes(storage, OLD_DATA "/settings.dat", bytes, sizeof(bytes));
    McApp* app = prompt();
    migrate(app);
    settle(app);
    assert(app->ui.cleanup_state == McCleanupFailed && !mc_cleanup_reviewed(storage));
    assert(!storage_file_exists(storage, NEW_DATA "/settings.dat"));
    check_bytes(storage, OLD_DATA "/settings.dat", bytes, sizeof(bytes));
    mc_app_free(app);
}

static void test_legacy_checkpoint_upgrade(void) {
    host_reset();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    assert(storage_common_mkdir(storage, OLD_DATA) == FSE_OK);
    McGame game;
    mc_game_start_run(&game, McDifficultyCommand, 321U);
    const uint32_t hash = mc_game_state_hash(&game);
    McRunSnapshot run = {.game = &game, .stage = McRunStageActive};
    uint8_t bytes[MC_RUN_ENCODED_MAX_SIZE + 24U];
    size_t size = mc_run_snapshot_encode(&run, bytes + 20U, MC_RUN_ENCODED_MAX_SIZE);
    assert(size);
    memcpy(bytes, "MCS1", 4);
    mc_write_u32(bytes + 4, 17U);
    mc_write_u32(bytes + 8, size);
    mc_write_u32(bytes + 12, 123456U);
    mc_write_u32(bytes + 16, mc_crc32(bytes, 16));
    write_bytes(storage, OLD_DATA "/save0.dat", bytes, size + 20U);
    McApp* app = prompt();
    migrate(app);
    settle(app);
    assert(app->ui.screen == McScreenTitle && mc_cleanup_reviewed(storage));
    check_bytes(storage, OLD_DATA "/save0.dat", bytes, size + 20U);
    memmove(bytes + 24U, bytes + 20U, size);
    memcpy(bytes, "MCS2", 4);
    mc_write_u32(bytes + 16, 0U);
    mc_write_u32(bytes + 20, mc_crc32(bytes, 20));
    check_bytes(storage, NEW_DATA "/save0.dat", bytes, size + 24U);
    McWaveStart wave;
    run.wave_start = &wave;
    app->persistence.slot = 0;
    assert(mc_persistence_restore(&app->persistence, &run) == McStorageOk);
    assert(app->persistence.generation == 17U && app->persistence.timestamp == 123456U);
    assert(mc_game_state_hash(&game) == hash && !wave.size);
    mc_app_free(app);
}

static void test_failures_and_retry(void) {
    // Fail every mutation in turn: mkdir, staging, writes, sync, publication, and review marker.
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
    for(unsigned validating = 0; validating < 2; validating++) {
        Storage* storage = fixture(false);
        McApp* app = prompt();
        migrate(app);
        bool interrupted = false;
        for(unsigned step = 0; step < 4096; step++) {
            mc_app_process_io(app, 0);
            if(storage_file_exists(storage, NEW_DATA "/nested/deeper/custom.bin") &&
               (!validating || app->cleanup.state == McCleanupValidating)) {
                interrupted = true;
                break;
            }
        }
        assert(interrupted);
        assert(!mc_cleanup_reviewed(storage));
        mc_app_free(app);
        app = prompt();
        migrate(app);
        settle(app);
        check_migrated(app, storage, false);
        mc_app_free(app);
    }
}

static void test_restart_during_upgrade(void) {
    for(unsigned completed = 1; completed < 3U + MC_SAVE_SLOTS; completed++) {
        Storage* storage = fixture(true);
        McApp* app = prompt();
        migrate(app);
        bool interrupted = false;
        for(unsigned step = 0; step < 4096; step++) {
            mc_app_process_io(app, 0);
            if(app->cleanup.state == McCleanupValidating && app->cleanup.validation == completed) {
                interrupted = true;
                break;
            }
        }
        assert(interrupted && !mc_cleanup_reviewed(storage));
        mc_app_free(app);
        app = prompt();
        migrate(app);
        settle(app);
        check_migrated(app, storage, true);
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

static void test_flat_directory_reads(void) {
    for(unsigned count = 20; count <= 40; count += 20) {
        host_reset();
        Storage* storage = furi_record_open(RECORD_STORAGE);
        assert(storage_common_mkdir(storage, OLD_DATA) == FSE_OK);
        for(unsigned i = 0; i < count; i++) {
            char path[128];
            snprintf(path, sizeof(path), OLD_DATA "/file%u", i);
            write_bytes(storage, path, payload, sizeof(payload));
        }
        McCleanup cleanup;
        mc_cleanup_init(&cleanup, storage);
        for(unsigned i = 0; i < 128 && cleanup.state == McCleanupScanning; i++)
            mc_cleanup_step(&cleanup);
        assert(cleanup.state == McCleanupPrompt && cleanup.source);
        memset(host_storage_calls, 0, sizeof(host_storage_calls));
        uint8_t scratch[MC_PERSISTENCE_SCRATCH_SIZE];
        mc_cleanup_migrate(&cleanup);
        for(unsigned i = 0; i < 2048 && cleanup.state == McCleanupMigrating; i++)
            mc_cleanup_migration_step(&cleanup, scratch);
        assert(cleanup.state == McCleanupValidating);
        assert(host_storage_calls[HostStorageDirOpen] == 1);
        assert(host_storage_calls[HostStorageDirRead] == count + 1);
        for(unsigned i = 0; i < count; i++) {
            char path[128];
            snprintf(path, sizeof(path), NEW_DATA "/file%u", i);
            check_bytes(storage, path, payload, sizeof(payload));
        }
        printf(
            "Migration: %u files, %u directory reads, one directory open\n",
            count,
            host_storage_calls[HostStorageDirRead]);
        mc_cleanup_deinit(&cleanup);
    }
}

static void settle_management(McApp* app) {
    for(unsigned i = 0; i < 4096; i++) {
        mc_app_process_io(app, 0);
        assert(!app->cleanup.allocated || !app->persistence.history);
        if(atomic_load(&app->worker.state) != McIoIdle || app->startup_step) continue;
        if(app->ui.screen == McScreenCleanup &&
           (app->ui.cleanup_state == McCleanupPrompt || app->ui.cleanup_state == McCleanupFailed))
            return;
        if(app->ui.screen == McScreenSettings && !app->cleanup_requested &&
           !app->cleanup_management && !app->pending_io)
            return;
    }
    assert(!"Version data management did not settle");
}

static void open_management(McApp* app) {
    mc_app_open_settings(app, McScreenTitle);
    app->ui.menu_index = 3; // Data group
    send(app, InputKeyOk);
    assert(app->ui.screen == McScreenSettings);
    for(unsigned i = 0; i < 5 && app->ui.menu_index != McSettingsItemVersionData; i++)
        send(app, InputKeyDown);
    assert(app->ui.menu_index == McSettingsItemVersionData);
    send(app, InputKeyOk);
    assert(app->cleanup_requested);
}

static void test_review_choice(void) {
    Storage* storage = fixture(false);
    McApp* app = prompt();
    send(app, InputKeyBack);
    settle(app);
    assert(mc_cleanup_reviewed(storage));
    assert(app->ui.screen == McScreenTitle);
    assert(storage_file_exists(storage, OLD_DATA "/settings.bak"));
    mc_app_free(app);
    app = mc_app_alloc();
    settle(app);
    assert(app->ui.screen == McScreenTitle); // The choice survives a fresh app instance.
    mc_app_free(app);
    host_corrupt(NEW_DATA "/" MC_VERSION_REVIEW_FILE);
    app = prompt(); // Invalid marker must not suppress the prompt.
    send(app, InputKeyBack);
    settle(app);
    assert(mc_cleanup_reviewed(storage));
    mc_app_free(app);

    // Every failed marker mutation leaves the published marker absent and retryable.
    const HostStorageOperation failures[] = {
        HostStorageMkdir, HostStorageWrite, HostStorageSync, HostStorageRename};
    for(unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        storage = fixture(false);
        app = prompt();
        host_fail_next(failures[i]);
        send(app, InputKeyBack);
        settle(app);
        assert(app->ui.cleanup_state == McCleanupFailed);
        assert(!mc_cleanup_reviewed(storage));
        app->ui.menu_index = 1;
        send(app, InputKeyOk);
        settle(app);
        assert(mc_cleanup_reviewed(storage) && app->ui.screen == McScreenTitle);
        mc_app_free(app);
    }

    // A marker-only older folder is not an import source; its marker is never copied.
    host_reset();
    storage = furi_record_open(RECORD_STORAGE);
    assert(storage_common_mkdir(storage, OLD_DATA) == FSE_OK);
    write_bytes(storage, OLD_DATA "/" MC_VERSION_REVIEW_FILE, "MCV1", 4);
    write_bytes(storage, OLD_DATA "/" MC_VERSION_REVIEW_FILE ".tmp", "MCV1", 4);
    app = mc_app_alloc();
    settle(app);
    assert(app->ui.screen == McScreenCleanup && !app->ui.cleanup_can_migrate);
    assert(!mc_cleanup_reviewed(storage));
    mc_app_free(app);
}

static void test_management_cleanup(void) {
    Storage* storage = fixture(false);
    McApp* app = prompt();
    send(app, InputKeyBack);
    settle(app);
    for(unsigned i = 0; i < 256; i++)
        mc_app_process_io(app, 0);
    assert(app->persistence.history);
    app->ui.settings.sound = false;
    mc_app_settings_changed(app);
    open_management(app);
    host_defer_io(true);
    mc_app_process_io(app, 0);
    assert(app->worker.job.operation == McIoSaveSettings);
    assert(!app->cleanup.allocated && app->ui.screen == McScreenSettings);
    host_complete_io();
    host_defer_io(false);
    settle_management(app);
    assert(app->ui.screen == McScreenCleanup && !app->persistence.history);
    assert(app->ui.cleanup_count == 5); // Current, equal, newer, and unrelated folders excluded.
    const unsigned removes = host_storage_calls[HostStorageRemove];
    send(app, InputKeyRight);
    send(app, InputKeyRight);
    send(app, InputKeyOk); // Preview is followed by an explicit confirmation.
    assert(app->ui.cleanup_confirm && !app->cleanup_action && !app->ui.menu_index);
    send(app, InputKeyBack);
    assert(!app->ui.cleanup_confirm);
    assert(host_storage_calls[HostStorageRemove] == removes);
    send(app, InputKeyRight);
    send(app, InputKeyRight);
    send(app, InputKeyOk);
    send(app, InputKeyRight);
    send(app, InputKeyOk);
    settle_management(app);
    assert(app->ui.screen == McScreenSettings && app->ui.menu_index == McSettingsItemVersionData);
    assert(!app->ui.settings.sound && !app->cleanup.allocated);
    assert(storage_common_stat(storage, OLD_DATA, NULL) == FSE_NOT_EXIST);
    assert(storage_common_stat(storage, APP_DATA_PATH("0.8.0"), NULL) == FSE_NOT_EXIST);
    assert(storage_common_stat(storage, APP_DATA_PATH("v7"), NULL) == FSE_NOT_EXIST);
    check_bytes(storage, APP_DATA_PATH("2.0.0/newer"), "newer", 5);
    check_bytes(storage, APP_DATA_PATH(MC_DATA_FOLDER "+build.other/equal"), "equal", 5);
    check_bytes(storage, APP_DATA_PATH("notes/keep"), "notes", 5);
    assert(storage_file_exists(storage, MC_HELP_PATH) && mc_cleanup_reviewed(storage));
    mc_app_free(app);
}

static void test_management_import(void) {
    Storage* storage = fixture(false);
    McApp* app = prompt();
    send(app, InputKeyBack);
    settle(app);
    open_management(app);
    settle_management(app);
    assert(app->ui.cleanup_can_migrate);
    migrate(app);
    settle_management(app);
    assert(app->ui.screen == McScreenSettings && !app->cleanup_management);
    assert(app->ui.menu_index == McSettingsItemVersionData);
    assert(app->ui.profile.best_score == 1234 && app->ui.settings.cursor_speed == McCursorFast);
    for(unsigned i = 0; i < MC_SAVE_SLOTS; i++)
        assert(app->ui.slots[i].status == McStorageOk);
    assert(mc_cleanup_reviewed(storage) && storage_file_exists(storage, OLD_DATA "/settings.bak"));
    // Version management cannot reload data under a paused run.
    app->ui.settings_return_screen = McScreenPaused;
    send(app, InputKeyOk);
    assert(!app->cleanup_requested);
    mc_app_free(app);

    // A failed manual import revokes the old Keep marker and prompts again next launch.
    storage = fixture(false);
    app = prompt();
    send(app, InputKeyBack);
    settle(app);
    open_management(app);
    settle_management(app);
    host_fail_next(HostStorageSync);
    migrate(app);
    settle_management(app);
    assert(app->ui.cleanup_state == McCleanupFailed && !mc_cleanup_reviewed(storage));
    send(app, InputKeyBack);
    settle_management(app);
    assert(!mc_cleanup_reviewed(storage));
    mc_app_free(app);
    app = prompt();
    mc_app_free(app);
}

static void test_cleanup_failures(void) {
    for(int fault = 0; fault < 128; fault++) {
        Storage* storage = fixture(false);
        McApp* app = prompt();
        app->ui.menu_index = 2;
        send(app, InputKeyOk);
        send(app, InputKeyRight);
        host_fail_storage_after(fault);
        send(app, InputKeyOk);
        settle(app);
        const bool failed = app->ui.cleanup_state == McCleanupFailed;
        check_bytes(storage, APP_DATA_PATH("2.0.0/newer"), "newer", 5);
        check_bytes(storage, APP_DATA_PATH(MC_DATA_FOLDER "+build.other/equal"), "equal", 5);
        assert(storage_file_exists(storage, MC_HELP_PATH));
        host_fail_storage_after(-1);
        if(failed) {
            app->ui.menu_index = 1;
            send(app, InputKeyOk);
            settle(app);
        }
        assert(app->ui.screen == McScreenTitle);
        assert(storage_common_stat(storage, OLD_DATA, NULL) == FSE_NOT_EXIST);
        mc_app_free(app);
        if(!failed) return;
    }
    assert(!"Cleanup mutation fault sweep never finished");
}

static void test_management_request_lifetime(void) {
    fixture(false);
    McApp* app = prompt();
    send(app, InputKeyBack);
    settle(app);
    for(unsigned i = 0; i < 256; i++)
        mc_app_process_io(app, 0);
    assert(atomic_load(&app->worker.state) == McIoIdle);
    // Required history cannot surrender its allocation, even when other writes have settled.
    assert(mc_persistence_history_clear(&app->persistence));
    assert(!mc_persistence_history_release(&app->persistence));
    app->history_required = true;
    app->pending_io |= McPendingIoHistory;
    open_management(app);
    assert(!mc_app_version_data_ready(app));
    settle_management(app);
    assert(app->ui.screen == McScreenCleanup && !app->persistence.history);
    send(app, InputKeyBack);
    settle_management(app);
    // Moving away while a write is pending cancels the request to open management.
    app->ui.settings.sound = !app->ui.settings.sound;
    mc_app_settings_changed(app);
    open_management(app);
    send(app, InputKeyBack);
    mc_app_process_io(app, 0);
    assert(!app->cleanup_requested && !app->cleanup_management);
    // A failed old read must not block management of version data.
    app->failed_io |= McPendingIoSlots;
    open_management(app);
    settle_management(app);
    assert(app->ui.screen == McScreenCleanup);
    mc_app_free(app);
}

int main(void) {
    test_schema_upgrade();
    test_missing_configuration();
    test_upgrade_failures();
    test_unsupported_settings();
    test_legacy_checkpoint_upgrade();
    test_flat_directory_reads();
    test_selection();
    test_migrate_and_conflicts();
    test_failures_and_retry();
    test_read_failures();
    test_restart_after_publication();
    test_restart_during_upgrade();
    test_interruption_and_validation();
    test_review_choice();
    test_management_cleanup();
    test_management_import();
    test_cleanup_failures();
    test_management_request_lifetime();
    puts("migration selection, files, saves, conflicts, interruptions, and retry tests passed");
    return 0;
}
