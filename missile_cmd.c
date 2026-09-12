#include "app.h"

#include <furi.h>

int32_t missile_cmd_app(void* context) {
    UNUSED(context);
    return mc_app_run();
}
