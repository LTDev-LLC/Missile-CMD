#include "binary.h"
#include "persistence_codec.h"
#include "text.h"

#include "balance.h"
#include "collision.h"
#include "game.h"
#include "wave.h"

#include <limits.h>
#include <string.h>

#define MC_SETTINGS_MAGIC    0x4D435354UL
#define MC_SCORES_MAGIC      0x4D435343UL
#define MC_PROFILE_MAGIC     0x4D435052UL
#define MC_RUN_MAGIC         0x4D435255UL
#define MC_SETTINGS_VERSION  3U
#define MC_SCORES_VERSION    1U
#define MC_PROFILE_VERSION   2U
#define MC_RUN_VERSION       2U
#define MC_HEADER_SIZE       8U
#define MC_CRC_SIZE          4U
#define MC_SETTINGS_PAYLOAD  30U
#define MC_SCORES_PAYLOAD    495U
#define MC_PROFILE_PAYLOAD   (MC_PROFILE_ENCODED_SIZE - 12U)
#define MC_SCORE_RECORD_SIZE 33U

typedef struct {
    uint8_t* cursor;
    uint8_t* end;
    bool ok;
} McWriter;

typedef struct {
    const uint8_t* cursor;
    const uint8_t* end;
    bool ok;
} McReader;

static void mc_encode_header(uint8_t* output, uint32_t magic, uint16_t version, uint16_t payload) {
    mc_write_u32(output, magic);
    // Header: 4-byte magic, 2-byte version, 2-byte payload length; all little-endian
    mc_write_u16(output + 4U, version);
    mc_write_u16(output + 6U, payload);
}

// Reflected CRC-32 with polynomial 0xEDB88320

static bool mc_validate_blob(
    const uint8_t* input,
    size_t size,
    uint32_t magic,
    uint16_t version,
    uint16_t payload) {
    const size_t expected = MC_HEADER_SIZE + payload + MC_CRC_SIZE;
    return input && size == expected && mc_read_u32(input) == magic &&
           mc_read_u16(input + 4U) == version && mc_read_u16(input + 6U) == payload &&
           mc_crc32(input, expected - MC_CRC_SIZE) == mc_read_u32(input + expected - MC_CRC_SIZE);
}

static bool mc_validate_variable_blob(
    const uint8_t* input,
    size_t size,
    uint32_t magic,
    uint16_t version,
    uint16_t* payload) {
    if(!input || size < MC_HEADER_SIZE + MC_CRC_SIZE || mc_read_u32(input) != magic ||
       mc_read_u16(input + 4U) != version)
        return false;
    const uint16_t declared = mc_read_u16(input + 6U);
    if(size != MC_HEADER_SIZE + (size_t)declared + MC_CRC_SIZE ||
       mc_crc32(input, size - MC_CRC_SIZE) != mc_read_u32(input + size - MC_CRC_SIZE))
        return false;
    if(payload) *payload = declared;
    return true;
}

static void mc_finish_blob(uint8_t* output, size_t size) {
    mc_write_u32(output + size - MC_CRC_SIZE, mc_crc32(output, size - MC_CRC_SIZE));
}

static void mc_writer_bytes(McWriter* writer, const void* data, size_t size) {
    // Keep overflow latched; later fields must not advance the cursor
    if(!writer->ok || size > (size_t)(writer->end - writer->cursor)) {
        writer->ok = false;
        return;
    }
    memcpy(writer->cursor, data, size);
    writer->cursor += size;
}

static __attribute__((noinline)) void mc_writer_u8(McWriter* writer, uint8_t value) {
    mc_writer_bytes(writer, &value, 1U);
}

static __attribute__((noinline)) void mc_writer_u16(McWriter* writer, uint16_t value) {
    uint8_t encoded[2];
    mc_write_u16(encoded, value);
    mc_writer_bytes(writer, encoded, sizeof(encoded));
}

static __attribute__((noinline)) void mc_writer_u32(McWriter* writer, uint32_t value) {
    uint8_t encoded[4];
    mc_write_u32(encoded, value);
    mc_writer_bytes(writer, encoded, sizeof(encoded));
}

static void mc_reader_bytes(McReader* reader, void* output, size_t size) {
    // Latch truncation and initialize requested output even after an earlier failure
    if(!reader->ok || size > (size_t)(reader->end - reader->cursor)) {
        reader->ok = false;
        if(output) memset(output, 0, size);
        return;
    }
    if(output) memcpy(output, reader->cursor, size);
    reader->cursor += size;
}

static __attribute__((noinline)) uint8_t mc_reader_u8(McReader* reader) {
    uint8_t value = 0U;
    mc_reader_bytes(reader, &value, 1U);
    return value;
}

static __attribute__((noinline)) uint16_t mc_reader_u16(McReader* reader) {
    uint8_t encoded[2] = {0U};
    mc_reader_bytes(reader, encoded, sizeof(encoded));
    return mc_read_u16(encoded);
}

static __attribute__((noinline)) uint32_t mc_reader_u32(McReader* reader) {
    uint8_t encoded[4] = {0U};
    mc_reader_bytes(reader, encoded, sizeof(encoded));
    return mc_read_u32(encoded);
}

// Descriptors pack a 12-bit object offset and a 4-bit wire width
// Width 3 is a one-byte boolean, validated before assignment
// Width 0 reserves one byte: write zero and discard on read
#define MC_RESERVED_BYTE 0U
#define MC_FIELD(type, field) \
    ((uint16_t)(offsetof(type, field) | (sizeof(((type*)0)->field) << 12)))
#define MC_BOOL(type, field)   ((uint16_t)(offsetof(type, field) | (3U << 12)))
#define MC_FIELD_COUNT(fields) (sizeof(fields) / sizeof((fields)[0]))
static void
    mc_encode_fields(McWriter* writer, const void* object, const uint16_t* fields, uint8_t count) {
    const unsigned char* bytes = object;
    for(uint8_t i = 0; i < count; i++) {
        const unsigned char* field = bytes + (fields[i] & 4095U);
        const uint8_t width = fields[i] >> 12;
        // Use aligned locals because fields may belong to packed structs
        if(width == 4) {
            uint32_t value;
            memcpy(&value, field, 4);
            mc_writer_u32(writer, value);
        } else if(width == 2) {
            uint16_t value;
            memcpy(&value, field, 2);
            mc_writer_u16(writer, value);
        } else
            mc_writer_u8(writer, width ? *field : 0U);
    }
}
static void
    mc_decode_fields(McReader* reader, void* object, const uint16_t* fields, uint8_t count) {
    unsigned char* bytes = object;
    for(uint8_t i = 0; i < count; i++) {
        unsigned char* field = bytes + (fields[i] & 4095U);
        const uint8_t width = fields[i] >> 12;
        if(width == 4) {
            uint32_t value = mc_reader_u32(reader);
            memcpy(field, &value, 4);
        } else if(width == 2) {
            uint16_t value = mc_reader_u16(reader);
            memcpy(field, &value, 2);
        } else {
            uint8_t value = mc_reader_u8(reader);
            if(width == 3 && value > 1) {
                reader->ok = false;
                return;
            }
            if(width) *field = value;
        }
    }
}

void mc_settings_defaults(McSettings* settings) {
    *settings = (McSettings){
        .cursor_speed = McCursorNormal,
        .difficulty = McDifficultyCommand,
        .render_mode = McRenderAdaptive,
        .sound = true,
        .vibration = true,
        .show_control_card = true,
        .reduced_flash = false,
        .trail_density = 2U,
        .cursor_acceleration = true,
        .strong_cursor = false,
        .vibration_intensity = 1U,
        .resume_countdown = true,
        .tap_pixels = 1U,
        .practice_aids = true,
    };
}

// Preserve the v2 byte-field order; struct padding is excluded
static const uint8_t McSettingOffsets[] = {
    offsetof(McSettings, sound),
    offsetof(McSettings, vibration),
    offsetof(McSettings, cursor_speed),
    offsetof(McSettings, difficulty),
    offsetof(McSettings, show_control_card),
    offsetof(McSettings, reduced_flash),
    offsetof(McSettings, render_mode),
    offsetof(McSettings, trail_density),
    offsetof(McSettings, cursor_acceleration),
    offsetof(McSettings, strong_cursor),
    offsetof(McSettings, vibration_intensity),
    offsetof(McSettings, led_mode),
    offsetof(McSettings, simple_hud),
    offsetof(McSettings, resume_countdown),
    offsetof(McSettings, tap_pixels),
    offsetof(McSettings, practice_aids),
    offsetof(McSettings, impact_warnings),
    offsetof(McSettings, last_setup_valid),
    offsetof(McSettings, last_mode),
    offsetof(McSettings, last_difficulty),
};
static bool mc_settings_payload_valid(const uint8_t* payload, const McRunOptions* options) {
    static const uint8_t maxima[] = {
        1,
        1,
        McCursorCount - 1,
        McDifficultyCount - 1,
        1,
        1,
        McRenderModeCount - 1,
        2,
        1,
        1,
        1,
        2,
        1,
        1,
        3,
        1,
        1,
        1,
        McModeCount - 1,
        McDifficultyCount - 1};
    for(uint8_t i = 0; i < sizeof(maxima); i++)
        if(payload[i] > maxima[i]) return false;
    return payload[7] != 1 && payload[14] != 0 && mc_options_valid(payload[18], options) &&
           (!payload[17] || mc_read_u32(payload + 20));
}
size_t mc_settings_encode(const McSettings* settings, uint8_t* output, size_t capacity) {
    if(!settings || !output || capacity < MC_SETTINGS_ENCODED_SIZE) return 0;
    uint8_t* payload = output + MC_HEADER_SIZE;
    const unsigned char* bytes = (const unsigned char*)settings;
    for(uint8_t i = 0; i < sizeof(McSettingOffsets); i++)
        payload[i] = bytes[McSettingOffsets[i]];
    mc_write_u32(payload + 20, settings->last_seed);
    mc_write_u16(payload + 24, settings->last_options.start_wave);
    payload[26] = settings->last_options.enemy;
    payload[27] = settings->last_options.slow;
    payload[28] = settings->last_options.unlimited;
    payload[29] = bytes[offsetof(McSettings, invert_colors)];
    if(!mc_settings_payload_valid(payload, &settings->last_options) || payload[29] > 1U) return 0;
    mc_encode_header(output, MC_SETTINGS_MAGIC, MC_SETTINGS_VERSION, MC_SETTINGS_PAYLOAD);
    mc_finish_blob(output, MC_SETTINGS_ENCODED_SIZE);
    return MC_SETTINGS_ENCODED_SIZE;
}
bool mc_settings_decode(McSettings* settings, const uint8_t* input, size_t size) {
    // Version 3 appends the display preference; version 2 keeps its original defaults.
    const bool legacy = mc_validate_blob(input, size, MC_SETTINGS_MAGIC, 2U, 29U);
    if(!settings ||
       (!legacy && !mc_validate_blob(
                       input, size, MC_SETTINGS_MAGIC, MC_SETTINGS_VERSION, MC_SETTINGS_PAYLOAD)))
        return false;
    const uint8_t* payload = input + MC_HEADER_SIZE;
    const McRunOptions options = {
        .start_wave = mc_read_u16(payload + 24),
        .enemy = payload[26],
        .slow = payload[27],
        .unlimited = payload[28]};
    // Validate raw boolean bytes before storing them in bool fields
    if(!mc_settings_payload_valid(payload, &options) || (!legacy && payload[29] > 1U))
        return false;
    // Start with current defaults so fields absent from older schemas are initialized.
    mc_settings_defaults(settings);
    unsigned char* bytes = (unsigned char*)settings;
    for(uint8_t i = 0; i < sizeof(McSettingOffsets); i++)
        bytes[McSettingOffsets[i]] = payload[i];
    settings->last_seed = mc_read_u32(payload + 20);
    settings->last_options = options;
    if(!legacy) settings->invert_colors = payload[29];
    return true;
}

void mc_score_tables_defaults(McScoreTables* scores) {
    memset(scores, 0, sizeof(*scores));
}

const McScoreBoard* mc_score_board(const McScoreTables* scores, McDifficulty difficulty) {
    return scores && difficulty < McDifficultyCount ? &scores->boards[difficulty] : NULL;
}

bool mc_score_tables_insert(McScoreTables* scores, const McScoreEntry* entry) {
    if(!scores || !entry || entry->difficulty >= McDifficultyCount || entry->score == 0U)
        return false;
    McScoreBoard* board = &scores->boards[entry->difficulty];
    size_t position = MC_SCORE_CAPACITY;
    for(size_t i = 0U; i < MC_SCORE_CAPACITY; i++) {
        if(entry->score > board->entries[i].score) {
            position = i;
            break;
        }
    }
    if(position == MC_SCORE_CAPACITY) return false;
    for(size_t i = MC_SCORE_CAPACITY - 1U; i > position; i--)
        board->entries[i] = board->entries[i - 1U];
    board->entries[position] = *entry;
    return true;
}

static const uint16_t McScoreFields[] = {
    MC_FIELD(McScoreEntry, score),
    MC_FIELD(McScoreEntry, timestamp),
    MC_FIELD(McScoreEntry, wave),
    MC_FIELD(McScoreEntry, difficulty),
    MC_FIELD(McScoreEntry, stats.shots_fired),
    MC_FIELD(McScoreEntry, stats.successful_shots),
    MC_FIELD(McScoreEntry, stats.enemies_destroyed),
    MC_FIELD(McScoreEntry, stats.play_ticks),
    MC_FIELD(McScoreEntry, stats.sites_lost),
    MC_FIELD(McScoreEntry, stats.sites_repaired),
    MC_FIELD(McScoreEntry, stats.max_chain_depth),
    MC_FIELD(McScoreEntry, cities_remaining),
};
static void mc_encode_score_record(uint8_t* record, const McScoreEntry* entry) {
    McWriter writer = {record, record + MC_SCORE_RECORD_SIZE, true};
    mc_encode_fields(&writer, entry, McScoreFields, MC_FIELD_COUNT(McScoreFields));
}
static bool mc_decode_score_record(McScoreEntry* entry, const uint8_t* record) {
    memset(entry, 0, sizeof(*entry));
    McReader reader = {record, record + MC_SCORE_RECORD_SIZE, true};
    mc_decode_fields(&reader, entry, McScoreFields, MC_FIELD_COUNT(McScoreFields));
    return reader.ok && entry->difficulty < McDifficultyCount &&
           entry->cities_remaining <= MC_CITY_COUNT &&
           entry->stats.successful_shots <= entry->stats.shots_fired;
}

size_t mc_score_tables_encode(const McScoreTables* scores, uint8_t* output, size_t capacity) {
    if(!scores || !output || capacity < MC_SCORES_ENCODED_SIZE) return 0U;
    mc_encode_header(output, MC_SCORES_MAGIC, MC_SCORES_VERSION, MC_SCORES_PAYLOAD);
    uint8_t* record = output + MC_HEADER_SIZE;
    for(uint8_t difficulty = 0U; difficulty < McDifficultyCount; difficulty++) {
        for(uint8_t i = 0U; i < MC_SCORE_CAPACITY; i++) {
            mc_encode_score_record(record, &scores->boards[difficulty].entries[i]);
            record += MC_SCORE_RECORD_SIZE;
        }
    }
    mc_finish_blob(output, MC_SCORES_ENCODED_SIZE);
    return MC_SCORES_ENCODED_SIZE;
}

static bool mc_decode_score_tables(McScoreTables* scores, const uint8_t* input, size_t size) {
    if(!mc_validate_blob(input, size, MC_SCORES_MAGIC, MC_SCORES_VERSION, MC_SCORES_PAYLOAD))
        return false;
    McScoreTables decoded;
    mc_score_tables_defaults(&decoded);
    const uint8_t* record = input + MC_HEADER_SIZE;
    for(uint8_t difficulty = 0U; difficulty < McDifficultyCount; difficulty++) {
        uint32_t prior = UINT32_MAX;
        for(uint8_t i = 0U; i < MC_SCORE_CAPACITY; i++) {
            McScoreEntry* entry = &decoded.boards[difficulty].entries[i];
            if(!mc_decode_score_record(entry, record) || entry->score > prior ||
               (entry->score > 0U && entry->difficulty != difficulty))
                return false;
            prior = entry->score;
            record += MC_SCORE_RECORD_SIZE;
        }
    }
    *scores = decoded;
    return true;
}

bool mc_score_tables_decode(McScoreTables* scores, const uint8_t* input, size_t size) {
    return scores && mc_decode_score_tables(scores, input, size);
}

void mc_profile_defaults(McProfile* profile) {
    memset(profile, 0, sizeof(*profile));
    profile->pinned_medal = UINT8_MAX;
}

// Binary-search the scaled percentage to avoid 64-bit division
uint16_t mc_accuracy_x100(uint32_t hits, uint32_t shots) {
    if(shots == 0U) return 0U;
    if(hits >= shots) return 10000U;
    uint16_t low = 0U;
    uint16_t high = 10000U;
    const uint64_t scaled_hits = (uint64_t)hits * 10000U;
    while(low < high) {
        // Use the upper midpoint so the search advances when low and high are adjacent
        const uint16_t candidate = (uint16_t)((low + high + 1U) >> 1U);
        const uint64_t scaled_shots = (uint64_t)shots * candidate;
        if(scaled_shots <= scaled_hits)
            low = candidate;
        else
            high = candidate - 1U;
    }
    return low;
}

static void mc_profile_records(
    McProfile* profile,
    uint32_t score,
    uint16_t wave,
    uint32_t ticks,
    uint32_t shots,
    uint32_t hits,
    uint8_t chain,
    uint8_t streak) {
    if(score > profile->best_score) profile->best_score = score;
    if(wave > profile->best_wave) profile->best_wave = wave;
    if(ticks > profile->best_survival_ticks) profile->best_survival_ticks = ticks;
    if(chain > profile->best_chain) profile->best_chain = chain;
    if(streak > profile->best_perfect_streak) profile->best_perfect_streak = streak;
    // Require at least twenty shots so one lucky hit cannot set the accuracy record
    if(shots >= 20U) {
        const uint16_t accuracy = mc_accuracy_x100(hits, shots);
        if(accuracy > profile->best_accuracy_x100) profile->best_accuracy_x100 = accuracy;
    }
}

static uint16_t mc_award(McProfile* profile, McMedal medal) {
    const uint16_t bit = (uint16_t)(1U << medal);
    if(profile->earned_medals & bit) return 0U;
    profile->earned_medals |= bit;
    return bit;
}

// Milestones share thresholds with the progress screen; event-specific rules stay explicit.
static const uint8_t McMilestoneModes[] =
    {McModeDaily, McModeLimitedAmmo, McModeOneCity, McModeBarrage, McModePerfect, McModeCrisis};
static uint8_t mc_milestone_target(uint8_t index) {
    return index == 5U ? 20U : 5U;
}
static void mc_profile_ranked(McProfile* profile, const McGame* game, McProfileEvent event) {
    if(event == McProfileWaveStarted) {
        if(game->wave > profile->best_wave) profile->best_wave = game->wave;
    } else {
        const bool ended = event == McProfileRunEnded;
        mc_profile_records(
            profile,
            game->score,
            game->wave,
            game->stats.play_ticks,
            ended ? game->stats.shots_fired : 0U,
            ended ? game->stats.successful_shots : 0U,
            game->stats.max_chain_depth,
            game->stats.best_perfect_wave_streak);
        if(game->stats.max_chain_depth >= 5U) mc_award(profile, McMedalChainReaction);
        if(game->difficulty == McDifficultyCrisis && game->wave >= (ended ? 11U : 10U))
            mc_award(profile, McMedalCrisisCommander);
    }
    if(game->wave >= 10U) mc_award(profile, McMedalSurvivor);
}
uint16_t mc_profile_update_started(McProfile* profile, const McGame* game) {
    if(!profile || !game || !mc_game_ranked(game)) return 0;
    uint16_t before = profile->earned_medals;
    mc_profile_ranked(profile, game, McProfileWaveStarted);
    return profile->earned_medals & (uint16_t)~before;
}

uint16_t mc_profile_update_wave(McProfile* profile, const McGame* game) {
    if(!profile || !game) return 0U;
    const uint16_t before = profile->earned_medals;
    if(game->mode == McModeTraining) {
        if(game->wave >= 4U && mc_training_passed(game)) mc_award(profile, McMedalCadetGraduate);
        return profile->earned_medals & (uint16_t)~before;
    }
    if(!mc_game_competitive(game)) return 0U;
    for(uint8_t i = 0; i < sizeof(McMilestoneModes); i++)
        if(game->mode == McMilestoneModes[i] && game->wave >= mc_milestone_target(i))
            mc_award(profile, McMedalDailyDuty + i);
    // Special-mode medals are awarded above; shared records below require Classic or Seeded
    if(!mc_game_ranked(game)) return profile->earned_medals & (uint16_t)~before;
    mc_profile_ranked(profile, game, McProfileWaveCleared);
    if(game->wave >= 5U && game->stats.perfect_wave_streak > 0U)
        mc_award(profile, McMedalPerfectDefense);
    if(game->stats.perfect_wave_streak >= 5U) mc_award(profile, McMedalIronDome);
    if(game->wave >= 5U && mc_game_total_ammo(game) >= 15U)
        mc_award(profile, McMedalQuartermaster);
    if(mc_game_alive_cities(game) == 1U) mc_award(profile, McMedalLastStand);
    return (uint16_t)(profile->earned_medals & (uint16_t)~before);
}

uint16_t mc_profile_update_game_over(McProfile* profile, const McGame* game) {
    if(!profile || !game) return 0U;
    const uint16_t before = profile->earned_medals;
    if(!mc_game_competitive(game)) return 0U;
    if(game->mode == McModePuzzle && game->victory)
        profile->puzzles_completed[game->difficulty] |= 1U << game->options.start_wave;
    if(game->score > profile->mode_best[game->mode]) profile->mode_best[game->mode] = game->score;
    if(game->mode == McModeDaily) {
        if(profile->daily_seed != game->seed) {
            profile->daily_seed = game->seed;
            profile->daily_best = 0U;
        }
        if(game->score > profile->daily_best) profile->daily_best = game->score;
    }
    if(!mc_game_ranked(game)) return 0U;
    mc_profile_ranked(profile, game, McProfileRunEnded);
    if(game->stats.shots_fired >= 20U &&
       mc_accuracy_x100(game->stats.successful_shots, game->stats.shots_fired) >= 7500U)
        mc_award(profile, McMedalSharpshooter);
    return (uint16_t)(profile->earned_medals & (uint16_t)~before);
}

const char* mc_medal_name(McMedal medal) {
    static const char Names[] = "Chain Reaction\0"
                                "Sharpshooter\0"
                                "Perfect Defense\0"
                                "Iron Dome\0"
                                "Quartermaster\0"
                                "Survivor\0"
                                "Crisis Commander\0"
                                "Last Stand\0"
                                "Daily Duty\0"
                                "Economist\0"
                                "Lone City\0"
                                "Storm Keeper\0"
                                "Flawless\0"
                                "Crisis Veteran\0"
                                "Cadet Graduate";
    return medal < McMedalCount ? mc_text_at(Names, medal) : "Unknown";
}

static const uint16_t McProfileFields[] = {
    MC_FIELD(McProfile, best_score),
    MC_FIELD(McProfile, best_survival_ticks),
    MC_FIELD(McProfile, best_wave),
    MC_FIELD(McProfile, best_accuracy_x100),
    MC_FIELD(McProfile, earned_medals),
    MC_FIELD(McProfile, best_chain),
    MC_FIELD(McProfile, best_perfect_streak),
    MC_FIELD(McProfile, mode_best[0]),
    MC_FIELD(McProfile, mode_best[1]),
    MC_FIELD(McProfile, mode_best[2]),
    MC_FIELD(McProfile, mode_best[3]),
    MC_FIELD(McProfile, mode_best[4]),
    MC_FIELD(McProfile, mode_best[5]),
    MC_FIELD(McProfile, mode_best[6]),
    MC_FIELD(McProfile, mode_best[7]),
    MC_FIELD(McProfile, mode_best[8]),
    MC_FIELD(McProfile, mode_best[9]),
    MC_FIELD(McProfile, mode_best[10]),
    MC_FIELD(McProfile, mode_best[11]),
    MC_FIELD(McProfile, mode_best[12]),
    MC_FIELD(McProfile, mode_best[13]),
    MC_FIELD(McProfile, mode_best[14]),
    MC_FIELD(McProfile, daily_seed),
    MC_FIELD(McProfile, daily_best),
    MC_FIELD(McProfile, pinned_medal),
    MC_FIELD(McProfile, puzzles_completed[0]),
    MC_FIELD(McProfile, puzzles_completed[1]),
    MC_FIELD(McProfile, puzzles_completed[2]),
};
static bool mc_profile_valid(const McProfile* p) {
    if((p->pinned_medal != UINT8_MAX && p->pinned_medal >= McMedalCount) ||
       p->best_accuracy_x100 > 10000U ||
       (p->earned_medals & (uint16_t) ~((1U << McMedalCount) - 1U)))
        return false;
    for(uint8_t i = 0; i < McDifficultyCount; i++)
        if(p->puzzles_completed[i] >= (1U << MC_PUZZLE_COUNT)) return false;
    return true;
}
size_t mc_profile_encode(const McProfile* profile, uint8_t* output, size_t capacity) {
    if(!profile || !output || capacity < MC_PROFILE_ENCODED_SIZE || !mc_profile_valid(profile))
        return 0;
    mc_encode_header(output, MC_PROFILE_MAGIC, MC_PROFILE_VERSION, MC_PROFILE_PAYLOAD);
    McWriter writer = {
        output + MC_HEADER_SIZE, output + MC_HEADER_SIZE + MC_PROFILE_PAYLOAD, true};
    mc_encode_fields(&writer, profile, McProfileFields, MC_FIELD_COUNT(McProfileFields));
    mc_finish_blob(output, MC_PROFILE_ENCODED_SIZE);
    return MC_PROFILE_ENCODED_SIZE;
}
bool mc_profile_decode(McProfile* profile, const uint8_t* input, size_t size) {
    if(!profile ||
       !mc_validate_blob(input, size, MC_PROFILE_MAGIC, MC_PROFILE_VERSION, MC_PROFILE_PAYLOAD))
        return false;
    McProfile decoded = {0};
    McReader reader = {input + MC_HEADER_SIZE, input + MC_HEADER_SIZE + MC_PROFILE_PAYLOAD, true};
    mc_decode_fields(&reader, &decoded, McProfileFields, MC_FIELD_COUNT(McProfileFields));
    if(!reader.ok || !mc_profile_valid(&decoded)) return false;
    *profile = decoded;
    return true;
}

static const uint16_t McRunPrefix[] = {
    MC_FIELD(McGame, phase),
    MC_FIELD(McGame, difficulty),
    MC_FIELD(McGame, wave_pattern),
    MC_FIELD(McGame, modifier),
    MC_FIELD(McGame, next_modifier),
    MC_FIELD(McGame, battery_selection),
    MC_FIELD(McGame, priority_site),
    MC_FIELD(McGame, pattern_spawn_index),
    MC_FIELD(McGame, repair_credits),
    MC_FIELD(McGame, last_spawn_x),
    MC_FIELD(McGame, wave),
    MC_FIELD(McGame, spawn_cooldown),
    MC_FIELD(McGame, last_wave_bonus),
    MC_FIELD(McGame, alive_site_mask),
    MC_FIELD(McGame, wave_start_alive_mask),
    MC_FIELD(McGame, cursor_x_q8),
    MC_FIELD(McGame, cursor_y_q8),
    MC_FIELD(McGame, battery_ammo[0]),
    MC_FIELD(McGame, battery_ammo[1]),
    MC_FIELD(McGame, battery_ammo[2]),
    MC_FIELD(McGame, roster_remaining[0]),
    MC_FIELD(McGame, roster_remaining[1]),
    MC_FIELD(McGame, roster_remaining[2]),
    MC_FIELD(McGame, pending_remaining),
    MC_FIELD(McGame, score),
    MC_FIELD(McGame, rng_state),
    MC_FIELD(McGame, roster_rng_state),
    MC_FIELD(McGame, director_rng_state),
    MC_FIELD(McGame, next_repair_score),
    MC_FIELD(McGame, stats.shots_fired),
    MC_FIELD(McGame, stats.successful_shots),
    MC_FIELD(McGame, stats.enemies_destroyed),
    MC_FIELD(McGame, stats.play_ticks),
    MC_FIELD(McGame, stats.sites_lost),
    MC_FIELD(McGame, stats.sites_repaired),
    MC_FIELD(McGame, stats.max_chain_depth),
    MC_FIELD(McGame, stats.perfect_wave_streak),
    MC_FIELD(McGame, stats.best_perfect_wave_streak),
    MC_FIELD(McGame, enemy_mask),
    MC_FIELD(McGame, interceptor_mask),
    MC_FIELD(McGame, root_mask),
    MC_FIELD(McGame, chain_mask),
    MC_FIELD(McGame, impact_mask),
};
static const uint16_t McRunSuffix[] = {
    MC_FIELD(McGame, seed),
    MC_FIELD(McGame, mode),
    MC_FIELD(McGame, bonus_ammo),
    MC_FIELD(McGame, radius_boost),
    MC_FIELD(McGame, next_radius_boost),
    MC_FIELD(McGame, training_flags),
    MC_FIELD(McGame, options.start_wave),
    MC_FIELD(McGame, options.enemy),
    MC_FIELD(McGame, options.slow),
    MC_FIELD(McGame, options.unlimited),
    MC_FIELD(McGame, last_lost_site),
    MC_FIELD(McGame, last_lost_enemy),
    MC_BOOL(McGame, puzzle_failed),
    MC_FIELD(McGame, rules_version),
    MC_FIELD(McGame, supplies),
    MC_FIELD(McGame, next_supplies),
    MC_BOOL(McGame, victory),
    MC_FIELD(McGame, wave_stats.shots),
    MC_FIELD(McGame, wave_stats.hits),
    MC_FIELD(McGame, wave_stats.kills),
    MC_FIELD(McGame, wave_stats.chain),
};
enum {
    McFieldOffset_enemies = 0,
    McFieldCount_enemies = 11
};
enum {
    McFieldOffset_interceptors = 11,
    McFieldCount_interceptors = 9
};
enum {
    McFieldOffset_roots = 20,
    McFieldCount_roots = 5
};
enum {
    McFieldOffset_chains = 25,
    McFieldCount_chains = 5
};
enum {
    McFieldOffset_impacts = 30,
    McFieldCount_impacts = 4
};
static const uint16_t McObjectFields[] = {
    MC_FIELD(McEnemyMissile, path.x_q9),
    MC_FIELD(McEnemyMissile, path.y_q9),
    MC_FIELD(McEnemyMissile, path.vx_q9),
    MC_FIELD(McEnemyMissile, path.vy_q9),
    MC_FIELD(McEnemyMissile, path.remaining),
    MC_FIELD(McEnemyMissile, path.target_x),
    MC_FIELD(McEnemyMissile, path.target_y),
    MC_FIELD(McEnemyMissile, split_remaining),
    MC_FIELD(McEnemyMissile, target_site),
    MC_FIELD(McEnemyMissile, kind),
    MC_RESERVED_BYTE, // Former trail age; retain its position in v2 snapshots
    MC_FIELD(McInterceptor, path.x_q9),
    MC_FIELD(McInterceptor, path.y_q9),
    MC_FIELD(McInterceptor, path.vx_q9),
    MC_FIELD(McInterceptor, path.vy_q9),
    MC_FIELD(McInterceptor, path.remaining),
    MC_FIELD(McInterceptor, path.target_x),
    MC_FIELD(McInterceptor, path.target_y),
    MC_FIELD(McInterceptor, battery_site),
    MC_RESERVED_BYTE, // Former trail age; retain its position in v2 snapshots
    MC_FIELD(McRootExplosion, x),
    MC_FIELD(McRootExplosion, y),
    MC_FIELD(McRootExplosion, age),
    MC_FIELD(McRootExplosion, max_radius),
    MC_FIELD(McRootExplosion, flags),
    MC_FIELD(McChainExplosion, x),
    MC_FIELD(McChainExplosion, y),
    MC_FIELD(McChainExplosion, age),
    MC_FIELD(McChainExplosion, max_radius),
    MC_FIELD(McChainExplosion, chain_depth),
    MC_FIELD(McImpactEffect, x),
    MC_FIELD(McImpactEffect, y),
    MC_FIELD(McImpactEffect, age),
    MC_FIELD(McImpactEffect, max_radius),
};
typedef struct {
    uint16_t fields;
    uint16_t offset, mask;
    uint8_t stride, capacity, count;
} McCodecPool;
#define MC_POOL(name, mask)                                      \
    {McFieldOffset_##name,                                       \
     offsetof(McGame, name),                                     \
     MC_FIELD(McGame, mask),                                     \
     sizeof(((McGame*)0)->name[0]),                              \
     sizeof(((McGame*)0)->name) / sizeof(((McGame*)0)->name[0]), \
     McFieldCount_##name}
static const McCodecPool McCodecPools[] = {
    MC_POOL(enemies, enemy_mask),
    MC_POOL(interceptors, interceptor_mask),
    MC_POOL(roots, root_mask),
    MC_POOL(chains, chain_mask),
    MC_POOL(impacts, impact_mask),
};
static uint32_t mc_codec_pool_mask(const McGame* game, const McCodecPool* pool) {
    const unsigned char* field = (const unsigned char*)game + (pool->mask & 4095U);
    if((pool->mask >> 12) == 4) {
        uint32_t value;
        memcpy(&value, field, 4);
        return value;
    }
    return *field;
}

// Encode authoritative state and occupied objects; snapshot size varies with pool occupancy
size_t mc_run_snapshot_encode(const McRunSnapshot* run, uint8_t* output, size_t capacity) {
    if(!run || !output || capacity < MC_HEADER_SIZE + MC_CRC_SIZE ||
       capacity > MC_RUN_ENCODED_MAX_SIZE || !run->game || run->stage >= McRunStageCount ||
       !mc_game_validate(run->game))
        return 0U;
    McWriter writer = {
        .cursor = output + MC_HEADER_SIZE,
        .end = output + capacity - MC_CRC_SIZE,
        .ok = true,
    };
    const McGame* game = run->game;
    mc_writer_u8(&writer, run->stage);
    mc_encode_fields(&writer, game, McRunPrefix, MC_FIELD_COUNT(McRunPrefix));

    // Slot indices preserve simulation order; derived caches are omitted
    for(uint8_t p = 0; p < MC_FIELD_COUNT(McCodecPools); p++) {
        const McCodecPool* pool = &McCodecPools[p];
        const uint32_t mask = mc_codec_pool_mask(game, pool);
        for(uint8_t i = 0; i < pool->capacity; i++) {
            if(!(mask & (1UL << i))) continue;
            mc_writer_u8(&writer, i);
            mc_encode_fields(
                &writer,
                (const unsigned char*)game + pool->offset + i * pool->stride,
                McObjectFields + pool->fields,
                pool->count);
        }
    }
    mc_encode_fields(&writer, game, McRunSuffix, MC_FIELD_COUNT(McRunSuffix));
    for(uint8_t i = 0U; i < MC_PACE_WAVES; i++)
        mc_writer_u32(&writer, game->wave_scores[i]);
    for(uint8_t i = 0U; i < MC_ENEMY_CAPACITY; i++)
        if(game->enemy_mask & (1UL << i)) {
            mc_writer_u8(&writer, game->enemies[i].origin_x);
            mc_writer_u8(&writer, game->enemies[i].origin_y);
        }
    if(!writer.ok) return 0U;
    const size_t payload = (size_t)(writer.cursor - (output + MC_HEADER_SIZE));
    const size_t total = MC_HEADER_SIZE + payload + MC_CRC_SIZE;
    if(total > MC_RUN_ENCODED_MAX_SIZE || payload > UINT16_MAX) return 0U;
    mc_encode_header(output, MC_RUN_MAGIC, MC_RUN_VERSION, (uint16_t)payload);
    mc_write_u32(writer.cursor, mc_crc32(output, total - MC_CRC_SIZE));
    return total;
}

static bool mc_run_stage_matches(const McRunSnapshot* run) {
    // Workshop and result screens share a phase but resume at different stages
    return run->game &&
           ((run->stage == McRunStageActive && run->game->phase == McGamePhasePlaying) ||
            (run->stage != McRunStageActive && run->game->phase == McGamePhaseWaveResult));
}

static __attribute__((noinline)) bool
    mc_decode_run(McRunSnapshot* run, const uint8_t* input, size_t size) {
    uint16_t payload = 0U;
    if(!mc_validate_variable_blob(input, size, MC_RUN_MAGIC, MC_RUN_VERSION, &payload))
        return false;
    if(!run->game) return false;
    mc_game_init(run->game);
    McReader reader = {
        .cursor = input + MC_HEADER_SIZE,
        .end = input + MC_HEADER_SIZE + payload,
        .ok = true,
    };
    McGame* game = run->game;
    run->stage = mc_reader_u8(&reader);
    mc_decode_fields(&reader, game, McRunPrefix, MC_FIELD_COUNT(McRunPrefix));

    for(uint8_t p = 0; p < MC_FIELD_COUNT(McCodecPools); p++) {
        const McCodecPool* pool = &McCodecPools[p];
        const uint32_t mask = mc_codec_pool_mask(game, pool);
        if(mask >> pool->capacity) return false;
        // Each occupied slot must appear exactly once, even if records arrive out of order
        uint32_t seen = 0;
        const uint8_t count = mc_popcount32(mask);
        for(uint8_t i = 0; i < count && reader.ok; i++) {
            const uint8_t index = mc_reader_u8(&reader);
            if(index >= pool->capacity || !(mask & (1UL << index)) || (seen & (1UL << index)))
                return false;
            seen |= 1UL << index;
            mc_decode_fields(
                &reader,
                (unsigned char*)game + pool->offset + index * pool->stride,
                McObjectFields + pool->fields,
                pool->count);
        }
        if(seen != mask) return false;
    }
    mc_decode_fields(&reader, game, McRunSuffix, MC_FIELD_COUNT(McRunSuffix));
    for(uint8_t i = 0U; i < MC_PACE_WAVES; i++)
        game->wave_scores[i] = mc_reader_u32(&reader);
    for(uint8_t i = 0U; i < MC_ENEMY_CAPACITY; i++)
        if(game->enemy_mask & (1UL << i)) {
            game->enemies[i].origin_x = mc_reader_u8(&reader);
            game->enemies[i].origin_y = mc_reader_u8(&reader);
            if(game->enemies[i].origin_x >= MC_SCREEN_WIDTH ||
               game->enemies[i].origin_y >= MC_SCREEN_HEIGHT)
                return false;
        }

    if(!reader.ok || reader.cursor != reader.end || run->stage >= McRunStageCount) return false;
    // Rebuild omitted caches before validating cross-field invariants
    mc_game_rebuild_derived(game);
    return game->stats.successful_shots <= game->stats.shots_fired && mc_game_validate(game) &&
           mc_run_stage_matches(run);
}

bool mc_run_snapshot_decode(McRunSnapshot* run, const uint8_t* input, size_t size) {
    return run && mc_decode_run(run, input, size);
}

void mc_medal_progress(
    const McProfile* profile,
    const McGame* game,
    uint8_t medal,
    uint16_t* value,
    uint16_t* target) {
    *value = 0U;
    *target = 1U;
    if(medal >= McMedalCount) return;
    switch(medal) {
    case McMedalChainReaction:
        *target = 5U;
        *value = profile->best_chain;
        break;
    case McMedalSharpshooter:
        *target = 75U;
        *value = profile->best_accuracy_x100 / 100U;
        if(mc_game_ranked(game) && game->stats.shots_fired >= 20U)
            *value =
                mc_accuracy_x100(game->stats.successful_shots, game->stats.shots_fired) / 100U;
        break;
    case McMedalPerfectDefense:
        *target = 5U;
        if(mc_game_ranked(game) && game->stats.perfect_wave_streak) *value = game->wave;
        break;
    case McMedalIronDome:
        *target = 5U;
        *value = profile->best_perfect_streak;
        break;
    case McMedalQuartermaster:
        *target = 15U;
        if(mc_game_ranked(game) && game->wave >= 5U) *value = mc_game_total_ammo(game);
        break;
    case McMedalSurvivor:
        *target = 10U;
        *value = profile->best_wave;
        break;
    case McMedalCrisisCommander:
        *target = 10U;
        if(mc_game_ranked(game) && game->difficulty == McDifficultyCrisis) *value = game->wave;
        break;
    case McMedalLastStand:
        if(mc_game_ranked(game) && mc_game_alive_cities(game) == 1U) *value = 1U;
        break;
    case McMedalCadetGraduate:
        *target = 4U;
        if(game->mode == McModeTraining)
            *value = game->wave - (mc_training_passed(game) ? 0U : 1U);
        break;
    default: {
        *target = mc_milestone_target(medal - McMedalDailyDuty);
        if(medal >= McMedalDailyDuty && medal <= McMedalCrisisVeteran &&
           game->mode == McMilestoneModes[medal - McMedalDailyDuty])
            *value = game->wave - (game->phase == McGamePhaseWaveResult ? 0U : 1U);
        break;
    }
    }
    if(profile->earned_medals & (1U << medal)) *value = *target;
    if(*value > *target) *value = *target;
}

#define MC_PACE_MAGIC   0x4150434DUL
#define MC_PACE_VERSION 1U
#define MC_PACE_PAYLOAD (4U + MC_PACE_WAVES * 4U)

size_t mc_pace_encode(const McPace* pace, uint8_t* output, size_t capacity) {
    if(!pace || !output || capacity < MC_PACE_ENCODED_SIZE) return 0U;
    for(uint8_t i = 0U; i < MC_PACE_WAVES; i++)
        if(pace->waves[i] > pace->score) return 0U;
    mc_encode_header(output, MC_PACE_MAGIC, MC_PACE_VERSION, MC_PACE_PAYLOAD);
    mc_write_u32(output + MC_HEADER_SIZE, pace->score);
    for(uint8_t i = 0U; i < MC_PACE_WAVES; i++)
        mc_write_u32(output + MC_HEADER_SIZE + 4U + i * 4U, pace->waves[i]);
    mc_finish_blob(output, MC_PACE_ENCODED_SIZE);
    return MC_PACE_ENCODED_SIZE;
}

bool mc_pace_decode(McPace* pace, const uint8_t* input, size_t size) {
    if(!pace || !mc_validate_blob(input, size, MC_PACE_MAGIC, MC_PACE_VERSION, MC_PACE_PAYLOAD))
        return false;
    McPace decoded = {.score = mc_read_u32(input + MC_HEADER_SIZE)};
    for(uint8_t i = 0U; i < MC_PACE_WAVES; i++) {
        decoded.waves[i] = mc_read_u32(input + MC_HEADER_SIZE + 4U + i * 4U);
        if(decoded.waves[i] > decoded.score) return false;
    }
    *pace = decoded;
    return true;
}

#define MC_WAVE_START_MAGIC 0x5357434DUL
// Reuse the authoritative run fields, omitting empty pools and prior-wave pace scores.
bool mc_wave_start_capture(McWaveStart* start, const McGame* game) {
    start->size = 0U;
    if(!mc_game_validate(game) || game->phase != McGamePhasePlaying || mc_game_has_motion(game))
        return false;
    McWriter writer = {
        start->data + MC_HEADER_SIZE, start->data + sizeof(start->data) - MC_CRC_SIZE, true};
    mc_encode_fields(&writer, game, McRunPrefix, MC_FIELD_COUNT(McRunPrefix));
    mc_encode_fields(&writer, game, McRunSuffix, MC_FIELD_COUNT(McRunSuffix));
    if(!writer.ok) return false;
    start->size = (uint16_t)(writer.cursor - start->data + MC_CRC_SIZE);
    mc_encode_header(
        start->data, MC_WAVE_START_MAGIC, 1U, start->size - MC_HEADER_SIZE - MC_CRC_SIZE);
    mc_finish_blob(start->data, start->size);
    return true;
}
bool mc_wave_start_restore(const McWaveStart* start, McGame* game) {
    uint16_t payload;
    if(!start || start->size > MC_WAVE_START_MAX_SIZE ||
       !mc_validate_variable_blob(start->data, start->size, MC_WAVE_START_MAGIC, 1U, &payload))
        return false;
    mc_game_init(game);
    McReader reader = {start->data + MC_HEADER_SIZE, start->data + MC_HEADER_SIZE + payload, true};
    mc_decode_fields(&reader, game, McRunPrefix, MC_FIELD_COUNT(McRunPrefix));
    mc_decode_fields(&reader, game, McRunSuffix, MC_FIELD_COUNT(McRunSuffix));
    // Validate masks before deriving anything from untrusted state.
    if(!reader.ok || reader.cursor != reader.end || game->enemy_mask || game->interceptor_mask ||
       game->root_mask || game->chain_mask || game->impact_mask)
        return false;
    mc_game_rebuild_derived(game);
    return game->phase == McGamePhasePlaying && mc_game_validate(game);
}
bool mc_wave_start_matches(const McWaveStart* start, const McGame* game) {
    if(!start || !start->size) return true;
    McGame restored;
    return mc_wave_start_restore(start, &restored) && restored.seed == game->seed &&
           restored.wave == game->wave && restored.mode == game->mode &&
           restored.difficulty == game->difficulty;
}
