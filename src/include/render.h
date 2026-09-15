#pragma once

#include "app_model.h"

#include <gui/canvas.h>

#ifdef MC_HOST_TEST
extern unsigned mc_host_render_refreshes;
extern size_t mc_host_snapshot_bytes;
#endif

void mc_render(Canvas* canvas, const McRenderSnapshot* model);

void mc_render_snapshot(McRenderSnapshot* snapshot, const McUiModel* model);
