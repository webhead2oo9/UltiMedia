#pragma once

#include "libretro.h"

// Shared diagnostic logger. Routes messages through the frontend's log
// interface when one is available, and otherwise falls back to stderr (for
// example under the native test harness). Safe to call before initialization:
// the stderr fallback is installed by default. Each message should include its
// own trailing newline.
extern retro_log_printf_t core_log;

// Query the frontend log interface. Call once environ_cb is available
// (currently from retro_set_environment).
void core_log_init(retro_environment_t environ_cb);
