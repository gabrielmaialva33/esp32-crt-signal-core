#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "crt_timing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t crt_line_policy_sync_width(const crt_timing_profile_t *timing, crt_timing_line_type_t line_type);
bool crt_line_policy_has_burst(crt_timing_line_type_t line_type);

#ifdef __cplusplus
}
#endif
