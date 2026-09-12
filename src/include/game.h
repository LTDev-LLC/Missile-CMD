#pragma once

#include "game_model.h"
#include "mode.h"

void mc_game_init(McGame* game);
void mc_game_start_run(McGame* game, McDifficulty difficulty, uint32_t seed);
void mc_game_start_mode(McGame* game, McDifficulty difficulty, uint32_t seed, McGameMode mode);
uint32_t mc_daily_seed(uint32_t timestamp);
const char* mc_mode_name(McGameMode mode);
bool mc_game_buy_supply(McGame* game, McSupply supply);
bool mc_game_can_buy_supply(const McGame* game, McSupply supply);
typedef uint8_t McWorkshopResult;
enum {
    McWorkshopOk,
    McWorkshopUnavailable,
    McWorkshopNoCredits,
    McWorkshopIntact,
    McWorkshopBought,
    McWorkshopNoBattery,
    McWorkshopRepairFirst
};
McWorkshopResult mc_game_repair_reason(const McGame* game, uint8_t site);
McWorkshopResult mc_game_supply_reason(const McGame* game, McSupply supply);
const char* mc_workshop_reason(McWorkshopResult result);
bool mc_game_select_battery(McGame* game, McBatterySelection selection);
bool mc_game_ranked(const McGame* game);
bool mc_training_passed(const McGame* game);
void mc_game_begin_next_wave(McGame* game);
McGameChange mc_game_step(McGame* game, McGameEventBuffer* events);
McGameChange mc_game_move_cursor_q8(McGame* game, int16_t dx_q8, int16_t dy_q8);
McFireResult mc_game_fire(McGame* game);
bool mc_game_repair_site(McGame* game, uint8_t site_index);
void mc_game_events_clear(McGameEventBuffer* events);
void mc_game_event_push(McGameEventBuffer* events, McGameEventType type, uint8_t chain_depth);
void mc_game_rebuild_derived(McGame* game);

const McSiteDef* mc_game_site_def(uint8_t site_index);
bool mc_game_site_alive(const McGame* game, uint8_t site_index);
uint8_t mc_game_site_ammo(const McGame* game, uint8_t site_index);
uint8_t mc_game_cursor_x(const McGame* game);
uint8_t mc_game_cursor_y(const McGame* game);
uint8_t mc_game_alive_cities(const McGame* game);
uint8_t mc_game_alive_sites(const McGame* game);
uint8_t mc_game_total_ammo(const McGame* game);
int8_t mc_game_selected_battery(const McGame* game);
uint8_t mc_game_destroyed_sites(const McGame* game);
bool mc_game_has_motion(const McGame* game);
uint32_t mc_game_state_hash(const McGame* game);
bool mc_game_validate(const McGame* game);

// Irrelevant options are normalized to the mode defaults
void mc_game_start_config(
    McGame* game,
    McDifficulty difficulty,
    uint32_t seed,
    McGameMode mode,
    const McRunOptions* options);
bool mc_options_valid(McGameMode mode, const McRunOptions* options);
bool mc_game_competitive(const McGame* game);
const char* mc_fire_reason(McFireResult result);
// Zero means the selected battery or interceptor pool cannot currently fire
uint16_t mc_game_aim_ticks(const McGame* game);
uint16_t mc_game_impact_warnings(const McGame* game);
uint8_t mc_game_debrief_id(const McGame* game);
