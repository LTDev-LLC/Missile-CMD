#pragma once

#include "build_version.h"

#define MC_DATA_FOLDER       MC_APP_VERSION
#define MC_HELP_PATH         APP_DATA_PATH(MC_DATA_FOLDER "/" MC_HELP_FILE)
// Include the entire version in paths, including prerelease/build metadata.
#define MC_STORAGE_PATH_SIZE (sizeof(APP_DATA_PATH(MC_DATA_FOLDER "/")) + 64U)
