#include "storage_cleanup.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

static void mc_cleanup_close(McCleanup* c) {
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
    c->tail = c->target = NULL;
    c->count = c->remaining = 0U;
    c->state = McCleanupDone;
}

static void mc_cleanup_fail(McCleanup* c) {
    mc_cleanup_close(c);
    c->state = McCleanupFailed;
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
    if(c->approved) {
        c->root_length = 0U;
        c->state = McCleanupPurging;
    } else {
        Storage* storage = c->storage;
        mc_cleanup_deinit(c);
        mc_cleanup_init(c, storage);
    }
}

// Inspect one root entry per step to keep discovery responsive
static void mc_cleanup_scan_step(McCleanup* c) {
    if(!c->scan) {
        c->scan = storage_file_alloc(c->storage);
        // APP_DATA_PATH("") adds a trailing slash that directory open rejects on-device
        if(!storage_dir_open(c->scan, STORAGE_APP_DATA_PATH_PREFIX)) mc_cleanup_fail(c);
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
        c->state = c->count ? McCleanupPrompt : McCleanupDone;
        return;
    }
    if(!file_info_is_dir(&info) || !mc_cleanup_candidate(name)) return;
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
    folder->next = NULL;
    c->allocated += size;
    strcpy(folder->name, name);
    if(c->tail)
        c->tail->next = folder;
    else
        c->folders = folder;
    c->tail = folder;
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

void mc_cleanup_step(McCleanup* c) {
    if(c->state == McCleanupScanning)
        mc_cleanup_scan_step(c);
    else if(c->state == McCleanupPurging && c->approved)
        mc_cleanup_purge_step(c);
}
