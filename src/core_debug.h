#pragma once

#include <stdbool.h>

// Internal state accessors for native test harnesses.
int core_debug_get_track_count(void);
int core_debug_get_current_index(void);
bool core_debug_is_shuffle_enabled(void);
bool core_debug_is_paused(void);
const char *core_debug_get_current_track_path(void);
