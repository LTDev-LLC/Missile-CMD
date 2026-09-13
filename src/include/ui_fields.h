// Shared value fields, included inside the live and rendering model structures.
McCleanupState cleanup_state;
size_t cleanup_count, cleanup_remaining, cleanup_index;
uint16_t cleanup_offset, cleanup_length;
// Cleanup and help screens are mutually exclusive. Snapshots own their copy.
union {
    struct {
        char cleanup_name[41], cleanup_source[41];
    };
    McHelpPage help;
};
bool cleanup_approved, cleanup_limited, cleanup_can_migrate, cleanup_migrating;
bool cleanup_confirm;
McSettings settings;
McScreen screen;
McScreen settings_return_screen;
McScreen scores_return_screen;
McScreen detail_return_screen;
McScreen confirm_return_screen;
McConfirmAction confirm_action;
McDifficulty score_difficulty;
uint32_t tick;
uint32_t high_score;
uint32_t setup_seed;
McGameMode setup_mode;
uint16_t medal_notice_mask;
uint8_t menu_index;
uint8_t score_index;
uint8_t repair_site_index;
uint8_t record_index;
uint8_t medal_index;
// Seed and wave editors share the selected digit; edit_wave is one-based
uint8_t seed_digit;
uint16_t edit_wave;
uint8_t workshop_reason;
uint8_t supply_index;
uint8_t guide_page;
// Negative means no eligible firing site
int8_t cached_selected_battery;
char hud_text[40];
char footer_text[32];
char timer_text[8];
uint16_t cached_seconds;
// aim_ticks includes half-speed scaling; warning_sites is a living-site mask
uint16_t aim_ticks, warning_sites;
// Unscaled flight time cached by integer target, battery, and speed upgrade
uint16_t aim_duration;
uint8_t aim_x, aim_y, aim_speed;
int8_t aim_battery;
bool aim_valid;
char aim_text[10];
uint32_t cached_score, cached_high_score;
uint16_t cached_wave;
uint8_t cached_ammo[3], cached_threats, cached_selection;
bool hud_valid, cached_simple;
McRunOptions setup_options;
bool retry_seed;
bool remember_setup;
bool override_difficulty;
McDifficulty setup_difficulty;
uint8_t settings_group, settings_rows[4];
uint8_t countdown_ticks, shot_notice_ticks, shot_reason, stats_page;
uint8_t active_slot, duel_player;
McSaveInfo slots[MC_SAVE_SLOTS];
McDuelResult duel[2];
// Positive means ahead of the saved pace for this wave
int32_t pace_delta;
bool pace_available;
// Storage labels must have static lifetime
const char* storage_item;
uint8_t storage_result;
bool storage_pending;
bool has_suspended_run;
bool storage_notice_visible;
bool new_high_score;
bool score_recorded;
bool storage_recovered;
bool battery_overlay;
bool wave_practice, has_wave_start, exit_pending, exit_slow, exit_discard;
