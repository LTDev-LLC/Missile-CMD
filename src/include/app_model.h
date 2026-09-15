#pragma once

#include "game.h"
#include "help.h"
#include "persistence_codec.h"
#include "persistence.h"
#include "storage_cleanup.h"

typedef uint8_t McScreen;
enum {
    McScreenTitle = 0,
    McScreenCleanup,
    McScreenControlCard,
    McScreenPlaying,
    McScreenPaused,
    McScreenWaveResult,
    McScreenRepair,
    McScreenGameOver,
    McScreenRunStats,
    McScreenHighScores,
    McScreenScoreDetails,
    McScreenRecords,
    McScreenMedals,
    McScreenMedalDetails,
    McScreenSettings,
    McScreenAbout,
    McScreenConfirm,
    McScreenRunSetup,
    McScreenSeed,
    McScreenLesson,
    McScreenModeRecords,
    McScreenHudGuide,
    McScreenPracticeSetup,
    McScreenSettingsGroups,
    McScreenStorage,
    McScreenSlots,
    McScreenDuelSwap,
    McScreenDuelResults,
    McScreenPuzzleHint,
    McScreenWaveEditor,
    McScreenLoading,
    McScreenPracticeResult,
    McScreenExiting,
};

typedef uint8_t McInputRecovery;
enum {
    McInputRecoveryNone = 0,
    McInputRecoveryHeld,
    McInputRecoveryReady,
};

typedef uint8_t McConfirmAction;
enum {
    McConfirmNone = 0,
    McConfirmOverwriteRun,
    McConfirmRestartRun,
    McConfirmAbandonRun,
    McConfirmSaveTitle,
    McConfirmResetSettings,
    McConfirmResetScores,
    McConfirmClearPace,
    McConfirmExitUnsaved,
};

enum {
    McSettingsItemSound = 0,
    McSettingsItemVibration,
    McSettingsItemCursor,
    McSettingsItemRenderMode,
    McSettingsItemReducedFlash,
    McSettingsItemShowControls,
    McSettingsItemTrails,
    McSettingsItemAcceleration,
    McSettingsItemStrongCursor,
    McSettingsItemVibrationIntensity,
    McSettingsItemResetSettings,
    McSettingsItemResetScores,
    McSettingsItemLed,
    McSettingsItemSimpleHud,
    McSettingsItemCountdown,
    McSettingsItemTap,
    McSettingsItemStorage,
    McSettingsItemWarnings,
    McSettingsItemClearPace,
    McSettingsItemInvertColors,
    McSettingsItemVersionData,
    McSettingsItemCount,
};

#define MC_SETTINGS_VISIBLE_ITEMS 5U
#define MC_MENU_VISIBLE_ITEMS     5U

typedef struct {
    uint32_t score, ticks, shots, hits;
} McDuelResult;

typedef struct McUiCommon {
#include "ui_fields.h"
} McUiCommon;

typedef struct McUiModel {
    union {
        McUiCommon common;
        struct {
#include "ui_fields.h"
        };
    };
    McGame game;
    McScoreTables scores;
    McProfile profile;
} McUiModel;

// Menu tables replace object-pool storage, which menu renderers never inspect.
// The scalar game suffix stays available on both gameplay and menu screens.
typedef struct {
    union {
        McUiCommon common;
        struct {
#include "ui_fields.h"
        };
    };
    union {
        McGame game;
        struct {
            McScoreTables scores;
            McProfile profile;
        };
    };
} McRenderSnapshot;
_Static_assert(
    sizeof(McScoreTables) + sizeof(McProfile) <= offsetof(McGame, stats),
    "menu data must fit within the unused object pools");
_Static_assert(sizeof(McRenderSnapshot) <= 1536U, "render snapshot exceeds 1.5 KiB");
