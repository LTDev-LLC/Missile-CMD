#include "binary.h"
#include "text.h"
#include "persistence.h"
#include "game.h"

#include <stdio.h>
#include <furi_hal.h>
#include <stdlib.h>
#include <string.h>

enum {
    McFileSettings,
    McFileScores,
    McFileProfile,
    McFileCheckpoint0,
    McFileCheckpoint1,
    McFileCheckpoint2,
    McFilePace,
    McFileHistory
};
// Family indices also select trust bits; pace uses a dynamic filename
static const char McFileNames[] = "settings\0scores\0profile\0save0\0save1\0save2\0\0pace-index";

bool mc_persistence_migration_file(const char* name) {
    for(uint8_t key = McFileSettings; key <= McFileCheckpoint2; key++) {
        const char* base = mc_text_at(McFileNames, key);
        const size_t length = strlen(base);
        if(strncmp(name, base, length) || name[length] != '.') continue;
        const char* extension = name + length + 1U;
        if(!strcmp(extension, "dat") || !strcmp(extension, "bak") || !strcmp(extension, "tmp"))
            return true;
    }
    return false;
}

static void mc_history_free(McPersistence* p);
static McPaceHistory* mc_history_get(McPersistence* p);
static void mc_history_touch(McPersistence* p, const McGame* game, uint32_t score);

static McStorageResult mc_data_directory(McPersistence* p) {
    FileInfo info;
    const FS_Error result = storage_common_stat(p->storage, APP_DATA_PATH(MC_DATA_FOLDER), &info);
    if(result == FSE_OK) return file_info_is_dir(&info) ? McStorageOk : McStorageInvalid;
    if(result != FSE_NOT_EXIST) return McStorageIoError;
    return storage_common_mkdir(p->storage, APP_DATA_PATH(MC_DATA_FOLDER)) == FSE_OK ?
               McStorageOk :
               McStorageIoError;
}

typedef bool (*McDecodeBlob)(void* target, const uint8_t* data, size_t size);

bool mc_persistence_init(McPersistence* p, Storage* storage) {
    if(!p || !storage) return false;
    memset(p, 0, sizeof(*p));
    p->scratch = malloc(MC_PERSISTENCE_SCRATCH_SIZE);
    p->storage = storage;
    // No filesystem work on the application thread. Pace history is allocated
    // lazily by the worker, after startup cleanup releases its folder list.
    return p->scratch != NULL;
}

void mc_persistence_deinit(McPersistence* p) {
    if(!p) return;
    mc_history_free(p);
    free(p->scratch);
    memset(p, 0, sizeof(*p));
}

static void
    mc_storage_path(const McPersistence* p, char* path, uint8_t key, const char* extension) {
    snprintf(
        path,
        MC_STORAGE_PATH_SIZE,
        APP_DATA_PATH(MC_DATA_FOLDER "/%s.%s"),
        key == McFilePace ? p->pace_name : mc_text_at(McFileNames, key),
        extension);
}

static McStorageResult
    mc_read_blob(McPersistence* p, const char* path, uint8_t* data, size_t cap, size_t* size) {
    if(!p || !p->storage || !p->scratch) return McStorageIoError;
    const FS_Error status = storage_common_stat(p->storage, path, NULL);
    if(status != FSE_OK) return status == FSE_NOT_EXIST ? McStorageMissing : McStorageIoError;
    File* file = storage_file_alloc(p->storage);
    McStorageResult result = McStorageIoError;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const uint64_t length = storage_file_size(file);
        if(length == 0U || length > cap)
            result = McStorageInvalid;
        else if(storage_file_read(file, data, length) == length) {
            *size = (size_t)length;
            result = McStorageOk;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return result;
}

typedef McStorageResult (*McReadCopy)(McPersistence*, const char*, void*);
static McStorageResult mc_load_copies_from(
    McPersistence* p,
    uint8_t key,
    McReadCopy read,
    void* target,
    const char* folder) {
    if(!p || !p->scratch) return McStorageIoError;
    McStorageResult directory = mc_data_directory(p);
    if(directory != McStorageOk) return directory;
    p->trusted_primary &= (uint16_t) ~(1U << key);
    p->recovered = false;
    McStorageResult first = McStorageMissing;
    char path[512];
    for(uint8_t copy = 0; copy < 2; copy++) {
        const int length = snprintf(
            path,
            sizeof(path),
            APP_DATA_PATH("%s/%s.%s"),
            folder,
            key == McFilePace ? p->pace_name : mc_text_at(McFileNames, key),
            copy ? "bak" : "dat");
        if(length < 0 || (size_t)length >= sizeof(path)) return McStorageInvalid;
        McStorageResult result = read(p, path, target);
        if(result == McStorageOk) {
            if(!copy) p->trusted_primary |= (uint16_t)(1U << key);
            p->recovered = copy != 0;
            return result;
        }
        if(first == McStorageMissing) first = result;
    }
    return first;
}
static McStorageResult
    mc_load_copies(McPersistence* p, uint8_t key, McReadCopy read, void* target) {
    return mc_load_copies_from(p, key, read, target, MC_DATA_FOLDER);
}

// Current records, including backups, win as a family. Only absent families use older data.
static McStorageResult mc_migration_read(
    McPersistence* p,
    const char* source,
    uint8_t key,
    McReadCopy read,
    void* target) {
    McStorageResult result = mc_load_copies(p, key, read, target);
    if(result == McStorageMissing) {
        result = mc_load_copies_from(p, key, read, target, source);
        p->trusted_primary &= (uint16_t) ~(1U << key);
    }
    return result;
}
typedef struct {
    void* target;
    McDecodeBlob decode;
    uint8_t* data;
    size_t cap;
} McBlobRead;
static McStorageResult mc_blob_read_copy(McPersistence* p, const char* path, void* context) {
    McBlobRead* b = context;
    size_t size = 0;
    McStorageResult result = mc_read_blob(p, path, b->data, b->cap, &size);
    return result == McStorageOk && !b->decode(b->target, b->data, size) ? McStorageInvalid :
                                                                           result;
}
static McStorageResult mc_load_blob(
    McPersistence* p,
    uint8_t key,
    void* target,
    McDecodeBlob decode,
    uint8_t* data,
    size_t cap) {
    McBlobRead read = {target, decode, data, cap};
    return mc_load_copies(p, key, mc_blob_read_copy, &read);
}

// Sync the replacement before rotating files to preserve recovery after interruption
static McStorageResult mc_write_parts(
    McPersistence* p,
    uint8_t key,
    const uint8_t* data,
    size_t size,
    const uint8_t* second,
    size_t second_size,
    const uint8_t* third,
    size_t third_size,
    bool migrating) {
    if(!p || !p->storage || !p->scratch || !data || size == 0U) return McStorageIoError;
    const McStorageResult directory = mc_data_directory(p);
    if(directory != McStorageOk) return directory;
    char primary[MC_STORAGE_PATH_SIZE], backup[MC_STORAGE_PATH_SIZE],
        temporary[MC_STORAGE_PATH_SIZE];
    mc_storage_path(p, primary, key, "dat");
    mc_storage_path(p, backup, key, "bak");
    mc_storage_path(p, temporary, key, "tmp");
    File* file = storage_file_alloc(p->storage);
    const struct {
        const uint8_t* data;
        size_t size;
    } parts[] = {{data, size}, {second, second_size}, {third, third_size}};
    bool written = storage_file_open(file, temporary, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    for(uint8_t i = 0; written && i < 3; i++)
        if(parts[i].size)
            written = storage_file_write(file, parts[i].data, parts[i].size) == parts[i].size;
    if(written) written = storage_file_sync(file);
    if(!storage_file_close(file)) written = false;
    if(written && migrating) {
        // Verify the staged current-format bytes before replacing any existing primary.
        written = storage_file_open(file, temporary, FSAM_READ, FSOM_OPEN_EXISTING) &&
                  storage_file_size(file) == size + second_size + third_size;
        uint8_t verify[64];
        for(uint8_t i = 0; written && i < 3; i++) {
            for(size_t offset = 0; written && offset < parts[i].size;) {
                const size_t remaining = parts[i].size - offset;
                const size_t chunk = remaining < sizeof(verify) ? remaining : sizeof(verify);
                written = storage_file_read(file, verify, chunk) == chunk &&
                          !memcmp(verify, parts[i].data + offset, chunk);
                offset += chunk;
            }
        }
        if(!storage_file_close(file)) written = false;
    }
    storage_file_free(file);
    if(!written) return McStorageIoError;
    // Retain a validated current copy across interrupted publication. Migration also
    // prefers this backup over older source records when only the backup survives.
    if(p->trusted_primary & (1U << key)) {
        if(storage_common_rename(p->storage, primary, backup) != FSE_OK) return McStorageIoError;
        p->trusted_primary &= (uint16_t) ~(1U << key);
    }
    if(storage_common_rename(p->storage, temporary, primary) != FSE_OK) return McStorageIoError;
    p->trusted_primary |= (uint16_t)(1U << key);
    return McStorageOk;
}

static McStorageResult
    mc_write_blob(McPersistence* p, uint8_t key, const uint8_t* data, size_t size) {
    return mc_write_parts(p, key, data, size, NULL, 0U, NULL, 0U, false);
}

typedef size_t (*McEncodeBlob)(const void*, uint8_t*, size_t);
static McStorageResult mc_migrate_blob(
    McPersistence* p,
    const char* source,
    uint8_t key,
    void* target,
    McDecodeBlob decode,
    McEncodeBlob encode,
    size_t capacity) {
    McBlobRead read = {target, decode, p->scratch, capacity};
    const McStorageResult result = mc_migration_read(p, source, key, mc_blob_read_copy, &read);
    if(result != McStorageOk && result != McStorageMissing) return result;
    const size_t size = encode(target, p->scratch, MC_PERSISTENCE_SCRATCH_SIZE);
    return mc_write_parts(p, key, p->scratch, size, NULL, 0U, NULL, 0U, true);
}

// Typed adapters avoid calling through incompatible function-pointer types
static bool mc_load_settings_codec(void* target, const uint8_t* data, size_t size) {
    return mc_settings_decode(target, data, size);
}
static bool mc_load_scores_codec(void* target, const uint8_t* data, size_t size) {
    return mc_score_tables_decode(target, data, size);
}
static bool mc_load_profile_codec(void* target, const uint8_t* data, size_t size) {
    return mc_profile_decode(target, data, size);
}
McStorageResult mc_persistence_load_settings(McPersistence* p, McSettings* settings) {
    const McStorageResult result = mc_load_blob(
        p,
        McFileSettings,
        settings,
        mc_load_settings_codec,
        p ? p->scratch : NULL,
        MC_SETTINGS_ENCODED_SIZE);
    if(result != McStorageOk) mc_settings_defaults(settings);
    return result;
}
McStorageResult mc_persistence_load_scores(McPersistence* p, McScoreTables* scores) {
    const McStorageResult result = mc_load_blob(
        p,
        McFileScores,
        scores,
        mc_load_scores_codec,
        p ? p->scratch : NULL,
        MC_SCORES_ENCODED_SIZE);
    if(result != McStorageOk) mc_score_tables_defaults(scores);
    return result;
}
McStorageResult mc_persistence_load_profile(McPersistence* p, McProfile* profile) {
    const McStorageResult result = mc_load_blob(
        p,
        McFileProfile,
        profile,
        mc_load_profile_codec,
        p ? p->scratch : NULL,
        MC_PROFILE_ENCODED_SIZE);
    if(result != McStorageOk) mc_profile_defaults(profile);
    return result;
}
McStorageResult mc_persistence_save_settings(McPersistence* p, const McSettings* settings) {
    if(!p) return McStorageIoError;
    return mc_write_blob(
        p,
        McFileSettings,
        p->scratch,
        mc_settings_encode(settings, p->scratch, MC_PERSISTENCE_SCRATCH_SIZE));
}
McStorageResult mc_persistence_save_scores(McPersistence* p, const McScoreTables* scores) {
    if(!p) return McStorageIoError;
    return mc_write_blob(
        p,
        McFileScores,
        p->scratch,
        mc_score_tables_encode(scores, p->scratch, MC_PERSISTENCE_SCRATCH_SIZE));
}
McStorageResult mc_persistence_save_profile(McPersistence* p, const McProfile* profile) {
    if(!p) return McStorageIoError;
    return mc_write_blob(
        p,
        McFileProfile,
        p->scratch,
        mc_profile_encode(profile, p->scratch, MC_PERSISTENCE_SCRATCH_SIZE));
}
static size_t mc_save_settings_codec(const void* target, uint8_t* data, size_t capacity) {
    return mc_settings_encode(target, data, capacity);
}
static size_t mc_save_scores_codec(const void* target, uint8_t* data, size_t capacity) {
    return mc_score_tables_encode(target, data, capacity);
}
static size_t mc_save_profile_codec(const void* target, uint8_t* data, size_t capacity) {
    return mc_profile_encode(target, data, capacity);
}
McStorageResult
    mc_persistence_migrate_settings(McPersistence* p, const char* source, McSettings* settings) {
    mc_settings_defaults(settings);
    return mc_migrate_blob(
        p,
        source,
        McFileSettings,
        settings,
        mc_load_settings_codec,
        mc_save_settings_codec,
        MC_SETTINGS_ENCODED_SIZE);
}
McStorageResult
    mc_persistence_migrate_scores(McPersistence* p, const char* source, McScoreTables* scores) {
    mc_score_tables_defaults(scores);
    return mc_migrate_blob(
        p,
        source,
        McFileScores,
        scores,
        mc_load_scores_codec,
        mc_save_scores_codec,
        MC_SCORES_ENCODED_SIZE);
}
McStorageResult
    mc_persistence_migrate_profile(McPersistence* p, const char* source, McProfile* profile) {
    mc_profile_defaults(profile);
    return mc_migrate_blob(
        p,
        source,
        McFileProfile,
        profile,
        mc_load_profile_codec,
        mc_save_profile_codec,
        MC_PROFILE_ENCODED_SIZE);
}

// The generation header and run snapshot have independent checksums
static McStorageResult
    mc_checkpoint_write(McPersistence* p, const McRunSnapshot* run, bool migrating) {
    if(!p || !p->scratch || p->slot >= MC_SAVE_SLOTS) return McStorageInvalid;
    const size_t size = mc_run_snapshot_encode(run, p->scratch, MC_PERSISTENCE_SCRATCH_SIZE);
    if(!size) return McStorageInvalid;
    uint8_t header[24] = {'M', 'C', 'S', '2'};
    if(!mc_wave_start_matches(run->wave_start, run->game)) return McStorageInvalid;
    const size_t replay_size = run->wave_start ? run->wave_start->size : 0U;
    const uint32_t generation = p->generation + !migrating;
    const uint32_t fields[] = {
        generation,
        (uint32_t)size,
        migrating ? p->timestamp : furi_hal_rtc_get_timestamp(),
        (uint32_t)replay_size};
    for(uint8_t field = 0U; field < 4U; field++)
        mc_write_u32(header + 4U + field * 4U, fields[field]);
    const uint32_t crc = mc_crc32(header, 20U);
    mc_write_u32(header + 20U, crc);
    const McStorageResult result = mc_write_parts(
        p,
        McFileCheckpoint0 + p->slot,
        header,
        sizeof(header),
        p->scratch,
        size,
        replay_size ? run->wave_start->data : NULL,
        replay_size,
        migrating);
    if(result == McStorageOk) p->generation = generation;
    return result;
}
McStorageResult mc_persistence_checkpoint(McPersistence* p, const McRunSnapshot* run) {
    return mc_checkpoint_write(p, run, false);
}

typedef struct {
    uint32_t generation, length, timestamp, replay_size;
    uint8_t header_size;
} McCheckpointHeader;

// Parse both container versions without changing the snapshot or recovery contract.
static bool
    mc_checkpoint_header(const uint8_t* bytes, uint64_t file_size, McCheckpointHeader* header) {
    const bool v2 = !memcmp(bytes, "MCS2", 4);
    if(!v2 && memcmp(bytes, "MCS1", 4)) return false;
    header->header_size = v2 ? 24 : 20;
    header->generation = mc_read_u32(bytes + 4);
    header->length = mc_read_u32(bytes + 8);
    header->timestamp = mc_read_u32(bytes + 12);
    header->replay_size = v2 ? mc_read_u32(bytes + 16) : 0;
    return mc_read_u32(bytes + header->header_size - 4) ==
               mc_crc32(bytes, header->header_size - 4) &&
           header->length > 0 && header->length <= MC_RUN_ENCODED_MAX_SIZE &&
           header->replay_size <= MC_WAVE_START_MAX_SIZE &&
           file_size == header->header_size + header->length + header->replay_size;
}

// Accept a copy only when both its container and snapshot validate; otherwise try the backup
static McStorageResult mc_checkpoint_copy(McPersistence* p, const char* path, void* context) {
    McRunSnapshot* run = context;
    const FS_Error status = storage_common_stat(p->storage, path, NULL);
    if(status != FSE_OK) return status == FSE_NOT_EXIST ? McStorageMissing : McStorageIoError;
    File* file = storage_file_alloc(p->storage);
    McStorageResult result = McStorageIoError;
    uint8_t header[24];
    McCheckpointHeader decoded;
    if(run->wave_start) run->wave_start->size = 0U;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const uint64_t file_size = storage_file_size(file);
        result = McStorageInvalid;
        if(file_size >= 20U && storage_file_read(file, header, 20U) == 20U) {
            const bool v2 = !memcmp(header, "MCS2", 4U);
            if(v2 && storage_file_read(file, header + 20U, 4U) != 4U) {
                result = McStorageIoError;
            } else {
                if(mc_checkpoint_header(header, file_size, &decoded)) {
                    const uint32_t length = decoded.length, replay_size = decoded.replay_size;
                    if(storage_file_read(file, p->scratch, length) != length)
                        result = McStorageIoError;
                    else if(mc_run_snapshot_decode(run, p->scratch, length)) {
                        McWaveStart ignored;
                        McWaveStart* replay = run->wave_start ? run->wave_start : &ignored;
                        replay->size = replay_size;
                        if(replay_size &&
                           storage_file_read(file, replay->data, replay_size) != replay_size)
                            result = McStorageIoError;
                        else if(mc_wave_start_matches(replay, run->game))
                            result = McStorageOk;
                    }
                }
            }
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    if(result == McStorageOk) {
        p->generation = decoded.generation;
        p->timestamp = decoded.timestamp;
    }
    return result;
}
static McStorageResult mc_checkpoint_read(McPersistence* p, uint8_t slot, McRunSnapshot* run) {
    return mc_load_copies(p, McFileCheckpoint0 + slot, mc_checkpoint_copy, run);
}

McStorageResult mc_persistence_restore(McPersistence* p, McRunSnapshot* run) {
    if(!p || !p->scratch || p->slot >= MC_SAVE_SLOTS) return McStorageInvalid;
    return mc_checkpoint_read(p, p->slot, run);
}

McStorageResult
    mc_persistence_migrate_run(McPersistence* p, const char* source, McRunSnapshot* run) {
    if(p->slot >= MC_SAVE_SLOTS) return McStorageInvalid;
    const McStorageResult result =
        mc_migration_read(p, source, McFileCheckpoint0 + p->slot, mc_checkpoint_copy, run);
    // An empty slot remains empty. Existing runs retain their generation and timestamp.
    return result == McStorageOk ? mc_checkpoint_write(p, run, true) : result;
}

static McStorageResult mc_remove_family(McPersistence* p, uint8_t key) {
    char path[MC_STORAGE_PATH_SIZE];
    // Delete the primary last so a leftover backup cannot revive a deleted run
    const char* extensions = "bak\0tmp\0dat";
    for(uint8_t i = 0U; i < 3U; i++) {
        mc_storage_path(p, path, key, mc_text_at(extensions, i));
        if(storage_file_exists(p->storage, path) &&
           storage_common_remove(p->storage, path) != FSE_OK)
            return McStorageIoError;
    }
    p->trusted_primary &= (uint16_t) ~(1U << key);
    return McStorageOk;
}
McStorageResult mc_persistence_remove_checkpoint(McPersistence* p) {
    if(!p || p->slot >= MC_SAVE_SLOTS) return McStorageInvalid;
    return mc_remove_family(p, McFileCheckpoint0 + p->slot);
}
static bool mc_load_pace_codec(void* target, const uint8_t* data, size_t size) {
    return mc_pace_decode(target, data, size);
}

McStorageResult
    mc_persistence_pace(McPersistence* p, const McGame* game, McPace* pace, bool save) {
    char name[48];
    const uint32_t seed = game->mode == McModeDaily || game->mode == McModeSeeded ? game->seed :
                                                                                    0U;
    snprintf(
        name,
        sizeof(name),
        "pace-%u-%u-%u-%u-%08lx",
        game->rules_version,
        game->mode,
        game->difficulty,
        game->options.start_wave,
        (unsigned long)seed);
    // Trust belongs to a filename; reset it when switching challenges
    if(strcmp(name, p->pace_name)) {
        p->trusted_primary &= (uint16_t) ~(1U << McFilePace);
        strcpy(p->pace_name, name);
    }
    if(!save) {
        memset(pace, 0, sizeof(*pace));
        return mc_load_blob(
            p, McFilePace, pace, mc_load_pace_codec, p->scratch, MC_PACE_ENCODED_SIZE);
    }
    if(!mc_game_competitive(game)) return McStorageInvalid;
    if((game->mode == McModeDaily || game->mode == McModeSeeded) && !mc_history_get(p))
        return McStorageIoError;
    const size_t size = mc_pace_encode(pace, p->scratch, MC_PERSISTENCE_SCRATCH_SIZE);
    const McStorageResult result = size ? mc_write_blob(p, McFilePace, p->scratch, size) :
                                          McStorageInvalid;
    if(result == McStorageOk) mc_history_touch(p, game, pace->score);
    return result;
}

#define MC_HISTORY_LIMIT 16U
// Index: 8-byte header, two groups of 16 nine-byte entries, 4-byte CRC
#define MC_HISTORY_SIZE  (12U + 2U * MC_HISTORY_LIMIT * 9U)
typedef struct {
    uint32_t seed, score;
    uint8_t difficulty;
} McHistoryEntry;
struct McPaceHistory {
    // Daily then Seeded; oldest-to-newest after saves, worst-to-best during import
    McHistoryEntry entries[2][MC_HISTORY_LIMIT], touch;
    // Phases: 0 load, 1-3 import, 4 publish, 5-7 prune, 8 done, 9 remove cleared index
    uint8_t counts[2], phase, touch_mode;
    // clearing remains set until history deletion finishes
    bool touching, clearing;
    File* scan;
    char candidate[64];
};
_Static_assert(
    sizeof(McPaceHistory) <= MC_HISTORY_ALLOCATION_BUDGET,
    "pace maintenance exceeds memory budget");

static McPaceHistory* mc_history_get(McPersistence* p) {
    if(!p->history) p->history = calloc(1U, sizeof(*p->history));
    return p->history;
}
static void mc_history_close(McPaceHistory* h) {
    if(h->scan) {
        storage_dir_close(h->scan);
        storage_file_free(h->scan);
        h->scan = NULL;
    }
}
static void mc_history_free(McPersistence* p) {
    if(p->history) {
        mc_history_close(p->history);
        free(p->history);
        p->history = NULL;
    }
}

static bool mc_history_decode(void* target, const uint8_t* b, size_t size) {
    if(size != MC_HISTORY_SIZE || memcmp(b, "MPH1", 4) || b[4] > 16 || b[5] > 16 || b[6] > 1 ||
       (b[6] && (b[4] || b[5])) || b[7] || mc_read_u32(b + size - 4) != mc_crc32(b, size - 4))
        return false;
    McPaceHistory* h = target;
    // Validate everything before changing retained entries
    for(unsigned mode = 0; mode < 2; mode++) {
        for(unsigned i = 0; i < b[4 + mode]; i++) {
            const uint8_t* entry = b + 8 + (mode * 16 + i) * 9;
            const uint32_t seed = mc_read_u32(entry);
            if(!seed || entry[8] >= McDifficultyCount) return false;
            for(unsigned j = 0; j < i; j++) {
                const uint8_t* prior = b + 8 + (mode * 16 + j) * 9;
                if(seed == mc_read_u32(prior) && entry[8] == prior[8]) return false;
            }
        }
    }
    h->clearing = b[6];
    for(unsigned mode = 0; mode < 2; mode++) {
        h->counts[mode] = b[4 + mode];
        for(unsigned i = 0; i < h->counts[mode]; i++) {
            const uint8_t* entry = b + 8 + (mode * 16 + i) * 9;
            h->entries[mode][i] =
                (McHistoryEntry){mc_read_u32(entry), mc_read_u32(entry + 4), entry[8]};
        }
    }
    return true;
}
static McStorageResult mc_history_write(McPersistence* p) {
    McPaceHistory* h = p->history;
    uint8_t* b = p->scratch;
    memset(b, 0, MC_HISTORY_SIZE);
    memcpy(b, "MPH1", 4);
    b[4] = h->counts[0];
    b[5] = h->counts[1];
    // Persist clear intent so interrupted deletion resumes after restart
    b[6] = h->clearing;
    for(unsigned mode = 0; mode < 2; mode++)
        for(unsigned i = 0; i < h->counts[mode]; i++) {
            uint8_t* entry = b + 8 + (mode * 16 + i) * 9;
            const McHistoryEntry* e = &h->entries[mode][i];
            mc_write_u32(entry, e->seed);
            mc_write_u32(entry + 4, e->score);
            entry[8] = e->difficulty;
        }
    mc_write_u32(b + MC_HISTORY_SIZE - 4, mc_crc32(b, MC_HISTORY_SIZE - 4));
    return mc_write_blob(p, McFileHistory, b, MC_HISTORY_SIZE);
}
// Accept only canonical names so pruning stays within this rules version
static bool mc_history_parse(const char* name, uint8_t* mode, McHistoryEntry* e, char* ext) {
    if(strncmp(name, "pace-", 5)) return false;
    const char* cursor = name + 5;
    uint8_t fields[4];
    for(unsigned field = 0; field < 4; field++) {
        const char* start = cursor;
        uint16_t value = 0;
        while(*cursor >= '0' && *cursor <= '9') {
            value = value * 10U + (unsigned)(*cursor++ - '0');
            if(value > 255) return false;
        }
        if(cursor == start || (*start == '0' && cursor - start > 1) || *cursor++ != '-')
            return false;
        fields[field] = value;
    }
    if(fields[0] != MC_RULES_VERSION || fields[1] >= McModeCount ||
       fields[2] >= McDifficultyCount ||
       (fields[1] == McModePuzzle ? fields[3] >= MC_PUZZLE_COUNT : fields[3]))
        return false;
    uint32_t seed = 0;
    for(unsigned digit = 0; digit < 8; digit++) {
        const char c = *cursor++;
        unsigned value;
        if(c >= '0' && c <= '9')
            value = c - '0';
        else if(c >= 'a' && c <= 'f')
            value = c - 'a' + 10;
        else
            return false;
        seed = (seed << 4) | value;
    }
    if(*cursor++ != '.' ||
       (strcmp(cursor, "dat") && strcmp(cursor, "bak") && strcmp(cursor, "tmp")))
        return false;
    if((fields[1] == McModeDaily || fields[1] == McModeSeeded) ? !seed : seed != 0) return false;
    strcpy(ext, cursor);
    *mode = fields[1];
    e->seed = seed;
    e->difficulty = fields[2];
    return true;
}
static int mc_history_find(const McPaceHistory* h, uint8_t mode, const McHistoryEntry* e) {
    for(unsigned i = 0; i < h->counts[mode]; i++)
        if(h->entries[mode][i].seed == e->seed && h->entries[mode][i].difficulty == e->difficulty)
            return (int)i;
    return -1;
}
// Import ties use canonical filename order for deterministic retention
static bool mc_history_better(const McHistoryEntry* a, const McHistoryEntry* b) {
    return a->score > b->score ||
           (a->score == b->score && (a->difficulty < b->difficulty ||
                                     (a->difficulty == b->difficulty && a->seed < b->seed)));
}
// Move existing identities to the end; a full list evicts the front before appending
static void mc_history_append(McPaceHistory* h, uint8_t mode, McHistoryEntry e) {
    int i = mc_history_find(h, mode, &e);
    if(i < 0 && h->counts[mode] == 16) i = 0;
    if(i >= 0) {
        memmove(
            &h->entries[mode][i], &h->entries[mode][i + 1], (h->counts[mode] - i - 1) * sizeof(e));
        h->counts[mode]--;
    }
    h->entries[mode][h->counts[mode]++] = e;
}
static void mc_history_touch(McPersistence* p, const McGame* game, uint32_t score) {
    if(game->mode != McModeDaily && game->mode != McModeSeeded) return;
    McPaceHistory* h = mc_history_get(p);
    if(!h) return;
    h->touch = (McHistoryEntry){game->seed, score, game->difficulty};
    h->touch_mode = game->mode == McModeSeeded;
    h->touching = true;
    // Index the new save before allowing pruning to continue
    if(h->phase >= 5) {
        mc_history_close(h);
        h->phase = 4;
    }
}
bool mc_persistence_history_pending(const McPersistence* p) {
    return !p->history || p->history->phase != 8;
}
bool mc_persistence_history_required(const McPersistence* p) {
    return p->history && (p->history->touching || p->history->phase == 4 || p->history->clearing);
}
bool mc_persistence_history_release(McPersistence* p) {
    if(mc_persistence_history_required(p)) return false;
    mc_history_free(p);
    return true;
}
bool mc_persistence_history_clear(McPersistence* p) {
    McPaceHistory* h = mc_history_get(p);
    if(!h) return false;
    mc_history_close(h);
    memset(h, 0, sizeof(*h));
    h->clearing = true;
    h->phase = 4;
    return true;
}
// Advance resumable history maintenance by one bounded step; Busy keeps it queued
McStorageResult mc_persistence_history_step(McPersistence* p) {
    McPaceHistory* h = mc_history_get(p);
    if(!h) return McStorageIoError;
    McStorageResult result;
    if(h->phase == 0) {
        result = mc_load_blob(p, McFileHistory, h, mc_history_decode, p->scratch, MC_HISTORY_SIZE);
        if(result == McStorageIoError) return result;
        // A backup index may be stale. Re-import surviving files before pruning
        // unless resuming a persisted clear intent
        const bool rebuild = result != McStorageOk || (p->recovered && !h->clearing);
        h->phase = rebuild ? 1 : (h->touching ? 4 : 5);
        if(rebuild) h->counts[0] = h->counts[1] = 0;
    } else if(h->phase == 1 || h->phase == 5) {
        h->scan = storage_file_alloc(p->storage);
        if(!storage_dir_open(h->scan, APP_DATA_PATH(MC_DATA_FOLDER))) {
            mc_history_close(h);
            return McStorageIoError;
        }
        h->phase++;
    } else if(h->phase == 2 || h->phase == 6) {
        FileInfo info;
        char name[256], ext[4];
        McHistoryEntry e = {0};
        uint8_t mode;
        if(storage_dir_read(h->scan, &info, name, sizeof(name))) {
            if(!file_info_is_dir(&info) && mc_history_parse(name, &mode, &e, ext)) {
                const bool seeded = mode == McModeDaily || mode == McModeSeeded;
                if(h->phase == 2 && seeded && strcmp(ext, "tmp")) {
                    strcpy(h->candidate, name);
                    h->phase = 3;
                } else if(
                    // Explicit clearing includes fixed modes; normal pruning covers seeded modes only
                    h->phase == 6 &&
                    (h->clearing ||
                     (seeded && mc_history_find(h, mode == McModeSeeded, &e) < 0))) {
                    strcpy(h->candidate, name);
                    h->phase = 7;
                }
            }
        } else {
            if(storage_file_get_error(h->scan) != FSE_NOT_EXIST) {
                mc_history_close(h);
                h->phase--;
                return McStorageIoError;
            }
            mc_history_close(h);
            if(h->phase == 2)
                h->phase = 4;
            else if(h->clearing)
                h->phase = 9;
            else
                h->phase = 8;
        }
    } else if(h->phase == 3) {
        // Import through a copy to preserve the live pace filename and primary trust
        McPersistence copy = *p;
        char ext[4];
        McHistoryEntry e = {0};
        uint8_t mode;
        if(!mc_history_parse(h->candidate, &mode, &e, ext)) return McStorageInvalid;
        strcpy(copy.pace_name, h->candidate);
        *strrchr(copy.pace_name, '.') = 0;
        McPace pace;
        result = mc_load_blob(
            &copy, McFilePace, &pace, mc_load_pace_codec, copy.scratch, MC_PACE_ENCODED_SIZE);
        if(result == McStorageIoError) return result;
        if(result == McStorageOk) {
            mode = mode == McModeSeeded;
            e.score = pace.score;
            if(mc_history_find(h, mode, &e) < 0 &&
               (h->counts[mode] < 16 || mc_history_better(&e, &h->entries[mode][0]))) {
                mc_history_append(h, mode, e);
                // Keep the worst import first so stronger entries replace it deterministically
                for(unsigned i = h->counts[mode] - 1;
                    i > 0 && mc_history_better(&h->entries[mode][i - 1], &h->entries[mode][i]);
                    i--) {
                    McHistoryEntry swap = h->entries[mode][i];
                    h->entries[mode][i] = h->entries[mode][i - 1];
                    h->entries[mode][i - 1] = swap;
                }
            }
        }
        h->phase = 2;
    } else if(h->phase == 4) {
        if(h->touching) {
            mc_history_append(h, h->touch_mode, h->touch);
            h->touching = false;
        }
        // Publish the index successfully before pruning any files
        result = mc_history_write(p);
        if(result != McStorageOk) return result;
        h->phase = 5;
    } else if(h->phase == 7) {
        char path[MC_STORAGE_PATH_SIZE];
        snprintf(path, sizeof(path), APP_DATA_PATH(MC_DATA_FOLDER "/%s"), h->candidate);
        const FS_Error error = storage_common_remove(p->storage, path);
        if(error != FSE_OK && error != FSE_NOT_EXIST) return McStorageIoError;
        p->trusted_primary &= (uint16_t) ~(1U << McFilePace);
        h->phase = 6;
    }
    // Delete the clear-intent index last to keep interrupted deletion resumable
    if(h->phase == 9) {
        result = mc_remove_family(p, McFileHistory);
        if(result != McStorageOk) return result;
        h->clearing = false;
        h->phase = 8;
    }
    return h->phase == 8 ? McStorageOk : McStorageBusy;
}

void mc_persistence_slot_info(McPersistence* p, uint8_t slot, McSaveInfo* info) {
    McGame game;
    McRunSnapshot run = {.game = &game};
    const uint8_t previous = p->slot;
    const uint32_t generation = p->generation, timestamp = p->timestamp;
    const bool recovered = p->recovered;
    p->slot = slot;
    memset(info, 0, sizeof(*info));
    info->status = mc_persistence_restore(p, &run);
    if(info->status == McStorageOk) {
        info->score = game.score;
        info->wave = game.wave;
        info->mode = game.mode;
        info->difficulty = game.difficulty;
        info->generation = p->generation;
        info->timestamp = p->timestamp;
    }
    p->slot = previous;
    p->generation = generation;
    p->timestamp = timestamp;
    p->recovered = recovered;
}
