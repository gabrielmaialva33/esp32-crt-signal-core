#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "crt_timing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t crt_timing_get_profile(crt_video_standard_t standard, crt_timing_profile_t *out_profile);
crt_timing_line_type_t crt_timing_get_line_type(crt_video_standard_t standard, uint16_t line_index);

#ifdef __cplusplus
}
#endif
