#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    McMenuTitle = 0,
    McMenuPause,
    McMenuGameOver,
} McMenuKind;

typedef enum {
    McMenuActionResume = 0,
    McMenuActionNewGame,
    McMenuActionHighScores,
    McMenuActionRecords,
    McMenuActionSettings,
    McMenuActionAbout,
    McMenuActionExit,
    McMenuActionContinue,
    McMenuActionSaveTitle,
    McMenuActionRestart,
    McMenuActionAbandon,
    McMenuActionRunStats,
    McMenuActionTitle,
    McMenuActionHudGuide,
    McMenuActionRetry,
    McMenuActionRandom,
    McMenuActionSlots,
    McMenuActionStorage,
    McMenuActionPlayLast,
    McMenuActionPracticeWave,
    McMenuActionPuzzleHint,
} McMenuAction;

#include "app_model.h"
uint8_t mc_menu_context(const McUiCommon* model, const McGame* game);

uint8_t mc_menu_count(McMenuKind menu, uint8_t context);
McMenuAction mc_menu_action(McMenuKind menu, uint8_t context, uint8_t index);
const char* mc_menu_label(McMenuAction action);

uint8_t mc_settings_group_count(uint8_t group);
uint8_t mc_settings_group_item(uint8_t group, uint8_t row);
const char* mc_settings_group_name(uint8_t group);

const char* mc_setting_label(uint8_t item);
const char* mc_setting_value(const McUiCommon* model, uint8_t item);
bool mc_setting_change(McUiCommon* model, uint8_t item, bool increment);
