#pragma once

#include "app_model.h"

#include <gui/canvas.h>

void mc_render(Canvas* canvas, const McRenderSnapshot* model);

void mc_render_snapshot(McRenderSnapshot* snapshot, const McUiModel* model);
