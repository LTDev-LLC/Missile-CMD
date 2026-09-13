#include "storage_cleanup.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MC_MIGRATION_TEMP APP_DATA_PATH(MC_DATA_FOLDER "/.migration.tmp")

static int mc_version_part(const char** left, const char** right, bool numeric) {
    const char *a = *left, *b = *right;
    while(**left && **left != '.' && **left != '-' && **left != '+')
        (*left)++;
    while(**right && **right != '.' && **right != '-' && **right != '+')
        (*right)++;
    const size_t al = *left - a, bl = *right - b;
    if(numeric && al != bl) return al > bl ? 1 : -1;
    const int order = strncmp(a, b, al < bl ? al : bl);
    return order ? order : (al > bl) - (al < bl);
}

int mc_cleanup_version_compare(const char* a, const char* b) {
    if(*a == 'v' || *b == 'v') {
        if(*a != *b) return *a == 'v' ? -1 : 1;
        a++;
        b++;
        while(*a == '0' && a[1])
            a++;
        while(*b == '0' && b[1])
            b++;
        return mc_version_part(&a, &b, true);
    }
    for(unsigned i = 0; i < 3; i++) {
        const int order = mc_version_part(&a, &b, true);
        if(order) return order;
        if(i < 2) {
            a++;
            b++;
        }
    }
    if(*a != '-' || *b != '-') return (*a != '-') - (*b != '-');
    a++;
    b++;
    for(;;) {
        const char *ae = a, *be = b;
        bool an = true, bn = true;
        while(*ae && *ae != '.' && *ae != '+') {
            if(*ae < '0' || *ae > '9') an = false;
            ae++;
        }
        while(*be && *be != '.' && *be != '+') {
            if(*be < '0' || *be > '9') bn = false;
            be++;
        }
        const size_t al = ae - a, bl = be - b;
        if(an != bn) return an ? -1 : 1;
        if(an && al != bl) return al > bl ? 1 : -1;
        const int order = strncmp(a, b, al < bl ? al : bl);
        if(order || al != bl) return order ? order : (al > bl ? 1 : -1);
        if(*ae != '.' || *be != '.') return (*ae == '.') - (*be == '.');
        a = ae + 1;
        b = be + 1;
    }
}

static bool mc_cleanup_older(const char* name) {
    return mc_cleanup_candidate(name) && mc_cleanup_version_compare(name, MC_DATA_FOLDER) < 0;
}

static bool mc_migration_asset(const char* name) {
    return !strcmp(name, ".migration.tmp") || !strcmp(name, MC_VERSION_REVIEW_FILE) ||
           !strcmp(name, MC_VERSION_REVIEW_FILE ".tmp") ||
           (!strncmp(name, MC_HELP_FILE, sizeof(MC_HELP_FILE) - 1U) &&
            (!name[sizeof(MC_HELP_FILE) - 1U] || name[sizeof(MC_HELP_FILE) - 1U] == '.'));
}

static bool mc_version_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool mc_version_number(const char** cursor) {
    const char* start = *cursor;
    while(mc_version_digit(**cursor))
        (*cursor)++;
    return *cursor != start && !(start[0] == '0' && *cursor - start > 1);
}

static bool mc_version_identifiers(const char** cursor, bool prerelease) {
    do {
        const char* start = ++(*cursor); // Skip the leading '-', '+' or '.'.
        bool numeric = true;
        while(mc_version_digit(**cursor) || (**cursor >= 'A' && **cursor <= 'Z') ||
              (**cursor >= 'a' && **cursor <= 'z') || **cursor == '-') {
            if(!mc_version_digit(**cursor)) numeric = false;
            (*cursor)++;
        }
        if(*cursor == start || (prerelease && numeric && start[0] == '0' && *cursor - start > 1))
            return false;
    } while(**cursor == '.');
    return true;
}

bool mc_cleanup_candidate(const char* name) {
    if(!name || !name[0] || !strcmp(name, MC_DATA_FOLDER)) return false;
    // Keep recognizing old numeric data folders so users can explicitly purge them.
    if(name[0] == 'v') {
        if(!name[1]) return false;
        for(size_t i = 1U; name[i]; i++)
            if(!mc_version_digit(name[i])) return false;
        return true;
    }
    const char* cursor = name;
    for(unsigned part = 0; part < 3U; part++) {
        if(!mc_version_number(&cursor)) return false;
        if(part < 2U && *cursor++ != '.') return false;
    }
    if(*cursor == '-' && !mc_version_identifiers(&cursor, true)) return false;
    if(*cursor == '+' && !mc_version_identifiers(&cursor, false)) return false;
    return !*cursor;
}

static void mc_cleanup_transfer_close(McCleanup* c) {
    if(c->input) {
        storage_file_close(c->input);
        storage_file_free(c->input);
        c->input = NULL;
    }
    if(c->output) {
        storage_file_close(c->output);
        storage_file_free(c->output);
        c->output = NULL;
    }
}

static void mc_cleanup_close(McCleanup* c) {
    mc_cleanup_transfer_close(c);
    if(c->scan) {
        storage_dir_close(c->scan);
        storage_file_free(c->scan);
        c->scan = NULL;
    }
}

void mc_cleanup_init(McCleanup* c, Storage* storage) {
    memset(c, 0, sizeof(*c));
    c->storage = storage;
    c->state = McCleanupScanning;
}

void mc_cleanup_deinit(McCleanup* c) {
    mc_cleanup_close(c);
    while(c->folders) {
        McCleanupFolder* folder = c->folders;
        c->folders = folder->next;
        c->allocated -= sizeof(*folder) + strlen(folder->name) + 1U;
        free(folder);
    }
    c->tail = c->target = c->source = NULL;
    c->count = c->remaining = 0U;
    c->state = McCleanupDone;
}

static void mc_cleanup_fail(McCleanup* c) {
    mc_cleanup_close(c);
    c->state = McCleanupFailed;
    c->result = McStorageIoError;
}

// A torn or invalid marker only causes another prompt; it can never authorize deletion.
static bool mc_review_file_valid(Storage* storage, const char* path) {
    File* file = storage_file_alloc(storage);
    if(!file) return false;
    char marker[4];
    const bool valid = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
                       storage_file_size(file) == sizeof(marker) &&
                       storage_file_read(file, marker, sizeof(marker)) == sizeof(marker) &&
                       !memcmp(marker, "MCV1", sizeof(marker));
    storage_file_close(file);
    storage_file_free(file);
    return valid;
}

bool mc_cleanup_reviewed(Storage* storage) {
    return mc_review_file_valid(storage, APP_DATA_PATH(MC_DATA_FOLDER "/" MC_VERSION_REVIEW_FILE));
}

static bool mc_cleanup_remember(McCleanup* c) {
    if(mc_cleanup_reviewed(c->storage)) return true;
    const FS_Error directory = storage_common_mkdir(c->storage, APP_DATA_PATH(MC_DATA_FOLDER));
    if(directory != FSE_OK && directory != FSE_EXIST) return false;
    File* file = storage_file_alloc(c->storage);
    if(!file) return false;
    bool saved = storage_file_open(
                     file,
                     APP_DATA_PATH(MC_DATA_FOLDER "/" MC_VERSION_REVIEW_FILE ".tmp"),
                     FSAM_WRITE,
                     FSOM_CREATE_ALWAYS) &&
                 storage_file_write(file, "MCV1", 4U) == 4U && storage_file_sync(file);
    if(!storage_file_close(file)) saved = false;
    storage_file_free(file);
    if(!saved || !mc_review_file_valid(
                     c->storage, APP_DATA_PATH(MC_DATA_FOLDER "/" MC_VERSION_REVIEW_FILE ".tmp")))
        return false;
    const FS_Error removed = storage_common_remove(
        c->storage, APP_DATA_PATH(MC_DATA_FOLDER "/" MC_VERSION_REVIEW_FILE));
    if(removed != FSE_OK && removed != FSE_NOT_EXIST) return false;
    return storage_common_rename(
               c->storage,
               APP_DATA_PATH(MC_DATA_FOLDER "/" MC_VERSION_REVIEW_FILE ".tmp"),
               APP_DATA_PATH(MC_DATA_FOLDER "/" MC_VERSION_REVIEW_FILE)) == FSE_OK;
}

void mc_cleanup_keep(McCleanup* c) {
    // Leaving a failed import must still offer recovery on the next launch.
    if(c->state != McCleanupFailed) {
        c->remembering = true;
        if(!mc_cleanup_remember(c)) {
            mc_cleanup_fail(c);
            return;
        }
    }
    c->state = McCleanupDone;
}

const char* mc_cleanup_name(const McCleanup* c, size_t index) {
    const McCleanupFolder* folder = c->folders;
    while(folder && index--)
        folder = folder->next;
    return folder ? folder->name : "";
}

void mc_cleanup_approve(McCleanup* c) {
    if(c->state != McCleanupPrompt) return;
    c->approved = true;
    c->target = c->folders;
    c->state = McCleanupPurging;
}

void mc_cleanup_retry(McCleanup* c) {
    if(c->state != McCleanupFailed) return;
    if(c->remembering) {
        if(mc_cleanup_remember(c)) c->state = McCleanupDone;
    } else if(c->migrating && !c->migrated) {
        c->root_length = 0U;
        c->resuming = c->verifying = false;
        c->validation = 0;
        c->state = McCleanupPrompt;
        mc_cleanup_migrate(c);
    } else if(c->approved) {
        c->root_length = 0U;
        c->state = McCleanupPurging;
    } else {
        Storage* storage = c->storage;
        mc_cleanup_deinit(c);
        mc_cleanup_init(c, storage);
    }
}

// The list is newest first. Empty/asset-only releases must not hide older user data.
static void mc_cleanup_select_source(McCleanup* c) {
    if(!c->target) {
        c->state = c->count ? McCleanupPrompt : McCleanupDone;
        return;
    }
    if(!mc_cleanup_older(c->target->name)) {
        c->target = c->target->next;
        return;
    }
    if(!c->scan) {
        snprintf(c->path, sizeof(c->path), APP_DATA_PATH("%s"), c->target->name);
        c->scan = storage_file_alloc(c->storage);
        if(!storage_dir_open(c->scan, c->path)) mc_cleanup_fail(c);
        return;
    }
    FileInfo info;
    char name[256];
    if(storage_dir_read(c->scan, &info, name, sizeof(name))) {
        if(!strcmp(name, ".") || !strcmp(name, "..") || mc_migration_asset(name)) return;
        c->source = c->target;
        mc_cleanup_close(c);
        c->state = McCleanupPrompt;
    } else if(storage_file_get_error(c->scan) != FSE_NOT_EXIST) {
        mc_cleanup_fail(c);
    } else {
        mc_cleanup_close(c);
        c->target = c->target->next;
    }
}

// Inspect one root entry per step to keep discovery responsive
static void mc_cleanup_scan_step(McCleanup* c) {
    if(c->selecting) {
        mc_cleanup_select_source(c);
        return;
    }
    if(!c->scan) {
        c->scan = storage_file_alloc(c->storage);
        // APP_DATA_PATH("") adds a trailing slash that directory open rejects on-device
        if(!storage_dir_open(c->scan, STORAGE_APP_DATA_PATH_PREFIX)) {
            if(storage_file_get_error(c->scan) == FSE_NOT_EXIST) {
                mc_cleanup_close(c);
                c->state = McCleanupDone;
            } else
                mc_cleanup_fail(c);
        }
        return;
    }
    FileInfo info;
    char name[256];
    if(!storage_dir_read(c->scan, &info, name, sizeof(name))) {
        if(storage_file_get_error(c->scan) != FSE_NOT_EXIST) {
            mc_cleanup_fail(c);
            return;
        }
        mc_cleanup_close(c);
        c->selecting = true;
        c->target = c->folders;
        return;
    }
    if(!file_info_is_dir(&info) || !mc_cleanup_older(name)) return;
    const size_t size = sizeof(McCleanupFolder) + strlen(name) + 1U;
    if(size > MC_CLEANUP_ALLOCATION_BUDGET - c->allocated) {
        c->limited = true;
        mc_cleanup_fail(c);
        return;
    }
    McCleanupFolder* folder = malloc(size);
    if(!folder) {
        mc_cleanup_fail(c);
        return;
    }
    c->allocated += size;
    strcpy(folder->name, name);
    McCleanupFolder** position = &c->folders;
    while(*position) {
        const int order = mc_cleanup_version_compare(name, (*position)->name);
        if(order > 0 || (!order && strcmp(name, (*position)->name) > 0)) break;
        position = &(*position)->next;
    }
    folder->next = *position;
    *position = folder;
    if(!folder->next) c->tail = folder;
    c->count++;
    c->remaining++;
}

static void mc_cleanup_target_done(McCleanup* c) {
    c->root_length = 0U;
    c->remaining--;
    c->target = c->target->next;
}

// Use one handle, reopening parents after child deletion. Removed entries cannot
// be visited again, and nesting uses no additional heap or stack
static void mc_cleanup_purge_step(McCleanup* c) {
    if(!c->target) {
        c->state = McCleanupDone;
        return;
    }
    if(!c->root_length) {
        if(!mc_cleanup_older(c->target->name)) {
            c->target = c->target->next;
            return;
        }
        if(!mc_cleanup_candidate(c->target->name)) {
            mc_cleanup_fail(c);
            return;
        }
        const int length =
            snprintf(c->path, sizeof(c->path), APP_DATA_PATH("%s"), c->target->name);
        if(length < 0 || (size_t)length >= sizeof(c->path)) {
            mc_cleanup_fail(c);
            return;
        }
        FileInfo info;
        const FS_Error status = storage_common_stat(c->storage, c->path, &info);
        if(status == FSE_NOT_EXIST) {
            mc_cleanup_target_done(c);
            return;
        }
        if(status != FSE_OK || !file_info_is_dir(&info)) {
            mc_cleanup_fail(c);
            return;
        }
        c->root_length = (size_t)length;
    }
    if(!c->scan) {
        c->scan = storage_file_alloc(c->storage);
        if(!storage_dir_open(c->scan, c->path)) mc_cleanup_fail(c);
        return;
    }
    char name[256];
    FileInfo info;
    if(storage_dir_read(c->scan, &info, name, sizeof(name))) {
        if(!strcmp(name, ".") || !strcmp(name, "..")) return;
        const size_t parent_length = strlen(c->path);
        const size_t length = strlen(name);
        // Reject separators so traversal cannot escape the approved root
        if(!length || strchr(name, '/') || strchr(name, '\\') ||
           parent_length + length + 2U > sizeof(c->path)) {
            mc_cleanup_fail(c);
            return;
        }
        c->path[parent_length] = '/';
        memcpy(c->path + parent_length + 1U, name, length + 1U);
        if(file_info_is_dir(&info)) {
            mc_cleanup_close(c);
        } else {
            const FS_Error status = storage_common_remove(c->storage, c->path);
            c->path[parent_length] = '\0';
            if(status != FSE_OK && status != FSE_NOT_EXIST) mc_cleanup_fail(c);
        }
        return;
    }
    if(storage_file_get_error(c->scan) != FSE_NOT_EXIST) {
        mc_cleanup_fail(c);
        return;
    }
    mc_cleanup_close(c);
    const FS_Error status = storage_common_remove(c->storage, c->path);
    if(status != FSE_OK && status != FSE_NOT_EXIST) {
        mc_cleanup_fail(c);
        return;
    }
    if(strlen(c->path) == c->root_length)
        mc_cleanup_target_done(c);
    else
        *strrchr(c->path, '/') = '\0';
}

void mc_cleanup_migrate(McCleanup* c) {
    if(c->state != McCleanupPrompt || !c->source) return;
    c->migrating = true;
    c->remembering = false;
    const FS_Error removed = storage_common_remove(
        c->storage, APP_DATA_PATH(MC_DATA_FOLDER "/" MC_VERSION_REVIEW_FILE));
    if(removed != FSE_OK && removed != FSE_NOT_EXIST) {
        mc_cleanup_fail(c);
        return;
    }
    c->root_length = 0;
    c->state = McCleanupMigrating;
}

static bool mc_migration_directory(McCleanup* c, const char* path) {
    FileInfo info;
    const FS_Error status = storage_common_stat(c->storage, path, &info);
    return status == FSE_OK ?
               file_info_is_dir(&info) :
               status == FSE_NOT_EXIST && storage_common_mkdir(c->storage, path) == FSE_OK;
}

static bool mc_migration_destination(McCleanup* c, char* path, size_t size) {
    const int length =
        snprintf(path, size, APP_DATA_PATH(MC_DATA_FOLDER "%s"), c->path + c->root_length);
    return length >= 0 && (size_t)length < size;
}

// File copies keep their parent's cursor. Only returning from a child directory needs
// to reopen the parent and find the child's name preserved after the terminator.
static void mc_migration_parent(McCleanup* c) {
    *strrchr(c->path, '/') = '\0';
    c->resuming = c->scan == NULL;
}

// Copy and read back at most 512 bytes per worker step, using the existing I/O scratch.
static void mc_migration_copy_step(McCleanup* c, uint8_t* scratch) {
    const size_t size = storage_file_read(c->input, scratch, 512U);
    if(storage_file_get_error(c->input) != FSE_OK) {
        mc_cleanup_fail(c);
        return;
    }
    if(c->verifying) {
        const size_t read = storage_file_read(c->output, scratch + 512U, size ? size : 1U);
        if(storage_file_get_error(c->output) != FSE_OK || read != size ||
           memcmp(scratch, scratch + 512U, size)) {
            mc_cleanup_fail(c);
            return;
        }
    } else if(size) {
        if(storage_file_write(c->output, scratch, size) != size) mc_cleanup_fail(c);
        return;
    }
    if(size) return;
    if(!c->verifying) {
        if(!storage_file_sync(c->output) || !storage_file_close(c->output) ||
           !storage_file_open(c->output, MC_MIGRATION_TEMP, FSAM_READ, FSOM_OPEN_EXISTING) ||
           !storage_file_seek(c->input, 0U, true)) {
            mc_cleanup_fail(c);
            return;
        }
        c->verifying = true;
        return;
    }
    mc_cleanup_transfer_close(c);
    char destination[512];
    FileInfo info;
    if(!mc_migration_destination(c, destination, sizeof(destination))) {
        mc_cleanup_fail(c);
        return;
    }
    // Never replace data already created in this release, including on a resumed migration.
    const FS_Error status = storage_common_stat(c->storage, destination, &info);
    if(status != FSE_NOT_EXIST ||
       storage_common_rename(c->storage, MC_MIGRATION_TEMP, destination) != FSE_OK) {
        mc_cleanup_fail(c);
        return;
    }
    c->verifying = false;
    mc_migration_parent(c);
}

void mc_cleanup_migration_step(McCleanup* c, uint8_t* scratch) {
    if(c->state != McCleanupMigrating || !c->source) return;
    if(c->input) {
        mc_migration_copy_step(c, scratch);
        return;
    }
    if(!c->root_length) {
        if(!mc_cleanup_older(c->source->name) ||
           !mc_migration_directory(c, APP_DATA_PATH(MC_DATA_FOLDER))) {
            mc_cleanup_fail(c);
            return;
        }
        const int length =
            snprintf(c->path, sizeof(c->path), APP_DATA_PATH("%s"), c->source->name);
        if(length < 0 || (size_t)length >= sizeof(c->path)) {
            mc_cleanup_fail(c);
            return;
        }
        c->root_length = length;
    }
    if(!c->scan) {
        c->scan = storage_file_alloc(c->storage);
        if(!storage_dir_open(c->scan, c->path)) mc_cleanup_fail(c);
        return;
    }
    char name[256];
    FileInfo info;
    const size_t parent = strlen(c->path);
    if(!storage_dir_read(c->scan, &info, name, sizeof(name))) {
        if(storage_file_get_error(c->scan) != FSE_NOT_EXIST || c->resuming) {
            mc_cleanup_fail(c);
            return;
        }
        mc_cleanup_close(c);
        if(parent == c->root_length) {
            const FS_Error status = storage_common_remove(c->storage, MC_MIGRATION_TEMP);
            if(status != FSE_OK && status != FSE_NOT_EXIST) {
                mc_cleanup_fail(c);
                return;
            }
            c->state = McCleanupValidating;
            c->validation = 0;
        } else
            mc_migration_parent(c);
        return;
    }
    if(c->resuming) {
        if(!strcmp(name, c->path + parent + 1U)) c->resuming = false;
        return;
    }
    if(!strcmp(name, ".") || !strcmp(name, "..")) return;
    if(parent == c->root_length &&
       (mc_migration_asset(name) || mc_persistence_migration_file(name)))
        return;
    const size_t length = strlen(name);
    if(!length || strchr(name, '/') || strchr(name, '\\') ||
       parent + length + 2U > sizeof(c->path)) {
        mc_cleanup_fail(c);
        return;
    }
    c->path[parent] = '/';
    memcpy(c->path + parent + 1U, name, length + 1U);
    char destination[512];
    if(!mc_migration_destination(c, destination, sizeof(destination))) {
        mc_cleanup_fail(c);
        return;
    }
    if(file_info_is_dir(&info)) {
        if(!mc_migration_directory(c, destination)) {
            mc_cleanup_fail(c);
            return;
        }
        mc_cleanup_close(c);
        return;
    }
    FileInfo existing;
    const FS_Error status = storage_common_stat(c->storage, destination, &existing);
    if(status == FSE_OK && !file_info_is_dir(&existing)) {
        c->path[parent] = '\0';
        return;
    }
    if(status != FSE_NOT_EXIST) {
        mc_cleanup_fail(c);
        return;
    }
    c->input = storage_file_alloc(c->storage);
    c->output = storage_file_alloc(c->storage);
    if(!storage_file_open(c->input, c->path, FSAM_READ, FSOM_OPEN_EXISTING) ||
       !storage_file_open(c->output, MC_MIGRATION_TEMP, FSAM_WRITE, FSOM_CREATE_ALWAYS))
        mc_cleanup_fail(c);
}

void mc_cleanup_validated(McCleanup* c, McStorageResult result) {
    if(c->state != McCleanupValidating) return;
    if(result != McStorageOk && result != McStorageMissing) {
        mc_cleanup_fail(c);
        c->result = result;
        return;
    }
    if(++c->validation < 3U + MC_SAVE_SLOTS) return;
    if(!mc_cleanup_remember(c)) {
        mc_cleanup_fail(c);
        return;
    }
    c->migrated = true;
    c->state = McCleanupDone;
}

void mc_cleanup_step(McCleanup* c) {
    if(c->state == McCleanupScanning)
        mc_cleanup_scan_step(c);
    else if(c->state == McCleanupPurging && c->approved)
        mc_cleanup_purge_step(c);
}
