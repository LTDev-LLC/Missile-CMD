#pragma once
#include "app_model.h"
uint8_t mc_help_requested(const McUiCommon* ui, const McGame* game);

// Called with the UI mutex held after each input and storage completion.
void mc_help_refresh(McUiCommon* ui, const McGame* game);
