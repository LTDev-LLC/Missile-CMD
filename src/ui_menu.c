#include "ui_menu.h"
#include "app_model.h"
#include "text.h"

#include <stddef.h>

static const uint8_t McTitleActions[] = {
    McMenuActionResume,
    McMenuActionNewGame,
    McMenuActionPlayLast,
    McMenuActionHighScores,
    McMenuActionRecords,
    McMenuActionSlots,
    McMenuActionHudGuide,
    McMenuActionSettings,
    McMenuActionAbout,
    McMenuActionExit,
};

static const uint8_t McPauseActions[] = {
    McMenuActionContinue,
    McMenuActionSaveTitle,
    McMenuActionRestart,
    McMenuActionPuzzleHint,
    McMenuActionSettings,
    McMenuActionHudGuide,
    McMenuActionAbandon,
    McMenuActionStorage,
};

static const uint8_t McGameOverActions[] = {
    McMenuActionRetry,
    McMenuActionPracticeWave,
    McMenuActionRandom,
    McMenuActionNewGame,
    McMenuActionRunStats,
    McMenuActionHighScores,
    McMenuActionTitle,
};

uint8_t mc_menu_context(const McUiCommon* model, const McGame* game) {
    // Context bits: Resume, Play last, Puzzle hint, Practice this wave
    return (model->has_suspended_run ? 1U : 0U) | (model->settings.last_setup_valid ? 2U : 0U) |
           (game->mode == McModePuzzle ? 4U : 0U) |
           (!game->victory && model->has_wave_start ? 8U : 0U);
}
static bool mc_menu_visible(McMenuAction action, uint8_t context) {
    const uint8_t flag = action == McMenuActionResume       ? 1U :
                         action == McMenuActionPlayLast     ? 2U :
                         action == McMenuActionPuzzleHint   ? 4U :
                         action == McMenuActionPracticeWave ? 8U :
                                                              0U;
    return !flag || (context & flag);
}
static const uint8_t* mc_menu_items(McMenuKind menu, uint8_t* count) {
    if(menu == McMenuTitle) {
        *count = sizeof(McTitleActions);
        return McTitleActions;
    }
    if(menu == McMenuPause) {
        *count = sizeof(McPauseActions);
        return McPauseActions;
    }
    *count = sizeof(McGameOverActions);
    return McGameOverActions;
}
uint8_t mc_menu_count(McMenuKind menu, uint8_t context) {
    uint8_t size, count = 0U;
    const uint8_t* items = mc_menu_items(menu, &size);
    for(uint8_t i = 0U; i < size; i++)
        if(mc_menu_visible(items[i], context)) count++;
    return count;
}
McMenuAction mc_menu_action(McMenuKind menu, uint8_t context, uint8_t index) {
    uint8_t size;
    const uint8_t* items = mc_menu_items(menu, &size);
    for(uint8_t i = 0U; i < size; i++) {
        if(!mc_menu_visible(items[i], context)) continue;
        if(!index--) return items[i];
    }
    return McMenuActionTitle;
}

const char* mc_menu_label(McMenuAction action) {
    // Labels must stay in action-enum order
    static const char Labels[] =
        "Resume Run\0"
        "New Game\0"
        "High Scores\0"
        "Records & Medals\0"
        "Settings\0"
        "About\0"
        "Exit\0"
        "Resume\0"
        "Save & Title\0"
        "Restart Run\0"
        "Abandon Run\0"
        "Run summary\0"
        "Title\0"
        "HUD Guide\0Retry challenge\0New random run\0Saved runs\0Save status\0Play last setup\0Practice this wave\0Puzzle hint";
    if((unsigned int)action > McMenuActionPuzzleHint) action = McMenuActionTitle;
    return mc_text_at(Labels, action);
}

static const uint8_t McSettingGroups[4][7] = {
    {McSettingsItemCursor,
     McSettingsItemTap,
     McSettingsItemAcceleration,
     McSettingsItemCountdown,
     McSettingsItemShowControls},
    {McSettingsItemRenderMode,
     McSettingsItemInvertColors,
     McSettingsItemTrails,
     McSettingsItemStrongCursor,
     McSettingsItemSimpleHud,
     McSettingsItemReducedFlash,
     McSettingsItemWarnings},
    {McSettingsItemSound,
     McSettingsItemVibration,
     McSettingsItemVibrationIntensity,
     McSettingsItemLed,
     McSettingsItemCount},
    {McSettingsItemStorage,
     McSettingsItemResetSettings,
     McSettingsItemResetScores,
     McSettingsItemClearPace,
     McSettingsItemCount},
};
uint8_t mc_settings_group_count(uint8_t group) {
    return group == 0U ? 5U : group == 1U ? 7U : group == 2U ? 4U : group == 3U ? 4U : 0U;
}
uint8_t mc_settings_group_item(uint8_t group, uint8_t row) {
    return row < mc_settings_group_count(group) ? McSettingGroups[group][row] :
                                                  McSettingsItemCount;
}
const char* mc_settings_group_name(uint8_t group) {
    return mc_text_at("Controls\0Display\0Feedback\0Data", group < 4U ? group : 0U);
}

// Byte offsets avoid pointers and repeated switches in settings metadata
typedef struct {
    uint8_t offset, count, scale, minimum, labels;
} McSettingDef;
#define SETTING(field, n, s, min, labels) {offsetof(McSettings, field), n, s, min, labels}
// count == 0 marks actions; scale and minimum preserve encoded setting values
static const McSettingDef McSettingDefs[] = {
    SETTING(sound, 2, 1, 0, 0),
    SETTING(vibration, 2, 1, 0, 0),
    SETTING(cursor_speed, 3, 1, 0, 2),
    SETTING(render_mode, 2, 1, 0, 5),
    SETTING(reduced_flash, 2, 1, 0, 0),
    SETTING(show_control_card, 2, 1, 0, 0),
    SETTING(trail_density, 2, 2, 0, 0),
    SETTING(cursor_acceleration, 2, 1, 0, 0),
    SETTING(strong_cursor, 2, 1, 0, 0),
    SETTING(vibration_intensity, 2, 1, 0, 7),
    {0},
    {0},
    SETTING(led_mode, 3, 1, 0, 9),
    SETTING(simple_hud, 2, 1, 0, 0),
    SETTING(resume_countdown, 2, 1, 0, 0),
    SETTING(tap_pixels, 3, 1, 1, 12),
    {0},
    SETTING(impact_warnings, 2, 1, 0, 0),
    {0},
    SETTING(invert_colors, 2, 1, 0, 0)};
#undef SETTING
const char* mc_setting_label(uint8_t item) {
    return mc_text_at(
        "Sound\0Vibration\0Cursor\0Render\0Reduced flash\0Show controls\0Missile tails\0Acceleration\0Bold cursor\0Vibration\0Reset settings\0Reset high scores\0LED\0Simple HUD\0Resume timer\0Tap distance\0Save status / retry\0Impact warnings\0Clear pace history\0Invert colors",
        item < McSettingsItemCount ? item : McSettingsItemResetSettings);
}
const char* mc_setting_value(const McUiCommon* model, uint8_t item) {
    if(item >= McSettingsItemCount || !McSettingDefs[item].count) return NULL;
    const McSettingDef* d = &McSettingDefs[item];
    const uint8_t value = *((const unsigned char*)&model->settings + d->offset);
    return mc_text_at(
        "Off\0On\0Slow\0Normal\0Fast\0Adaptive\0Battery Saver\0Light\0Normal\0All\0Critical\0Off\0"
        "1 px\0"
        "2 px\0"
        "3 px",
        d->labels + (value - d->minimum) / d->scale);
}
bool mc_setting_change(McUiCommon* model, uint8_t item, bool increment) {
    if(item >= McSettingsItemCount || !McSettingDefs[item].count) return false;
    const McSettingDef* d = &McSettingDefs[item];
    unsigned char* value = (unsigned char*)&model->settings + d->offset;
    const uint8_t index = (*value - d->minimum) / d->scale;
    *value = d->minimum + d->scale * (increment ? (index + 1U) % d->count :
                                                  (index ? index - 1U : d->count - 1U));
    return true;
}
