#include "help_ui.h"
#include "wave.h"

uint8_t mc_help_requested(const McUiCommon* ui, const McGame* game) {
    if(ui->exit_pending) return McHelpNoPage;
    switch(ui->screen) {
    case McScreenHudGuide:
        return McHelpHud0 + (ui->guide_page ? 1U : 0U);
    case McScreenLesson:
        return McHelpLesson0 + (game->wave > 4U ? 3U : game->wave ? game->wave - 1U : 0U);
    case McScreenPuzzleHint: {
        uint16_t puzzle = ui->detail_return_screen == McScreenPaused ?
                              game->options.start_wave :
                              ui->setup_options.start_wave;
        return McHelpPuzzle0 + (puzzle < MC_PUZZLE_COUNT ? puzzle : 0U);
    }
    case McScreenMedalDetails:
        return ui->medal_index < McMedalCount ? McHelpMedal0 + ui->medal_index : McHelpNoPage;
    case McScreenRunSetup:
        return ui->setup_mode < McModeCount && ui->setup_mode != McModeDaily ?
                   McHelpMode0 + ui->setup_mode :
                   McHelpNoPage;
    case McScreenRunStats:
        return ui->stats_page ? McHelpNoPage : McHelpDebrief0 + mc_game_debrief_id(game);
    default:
        return McHelpNoPage;
    }
}

void mc_help_refresh(McUiCommon* ui, const McGame* game) {
    // Cleanup owns the same storage until its worker finishes.
    if(ui->screen == McScreenCleanup) return;
    const uint8_t wanted = mc_help_requested(ui, game);
    if(wanted != ui->help.id || (wanted != McHelpNoPage && ui->help.status == McHelpEmpty)) {
        const uint32_t revision = ui->help.revision + 1U;
        ui->help = (McHelpPage){
            .revision = revision,
            .id = wanted,
            .status = wanted == McHelpNoPage ? McHelpEmpty : McHelpLoading};
    }
}
