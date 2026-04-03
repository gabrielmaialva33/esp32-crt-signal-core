#include "crt_timing.h"

#include "esp_check.h"

esp_err_t crt_timing_get_profile(crt_video_standard_t standard, crt_timing_profile_t *out_profile)
{
    ESP_RETURN_ON_FALSE(out_profile != NULL, ESP_ERR_INVALID_ARG, "crt_timing", "profile is null");

    switch (standard) {
    case CRT_VIDEO_STANDARD_NTSC:
        *out_profile = (crt_timing_profile_t) {
            .standard = CRT_VIDEO_STANDARD_NTSC,
            .sample_rate_hz = 14318180,
            .total_lines = 262,
            .active_lines = 240,
            .samples_per_line = 912,
            .active_offset = 144,
            .active_width = 768,
            .sync_width = 64,
            .vsync_width = 392,
            .burst_offset = 64,
            .burst_width = 40,
        };
        return ESP_OK;
    case CRT_VIDEO_STANDARD_PAL:
        *out_profile = (crt_timing_profile_t) {
            .standard = CRT_VIDEO_STANDARD_PAL,
            .sample_rate_hz = 17734476,
            .total_lines = 312,
            .active_lines = 240,
            .samples_per_line = 1136,
            .active_offset = 184,
            .active_width = 768,
            .sync_width = 80,
            .vsync_width = 536,
            .burst_offset = 96,
            .burst_width = 44,
        };
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

crt_timing_line_type_t crt_timing_get_line_type(crt_video_standard_t standard, uint16_t line_index)
{
    switch (standard) {
    case CRT_VIDEO_STANDARD_NTSC:
        if (line_index < 240) {
            return CRT_TIMING_LINE_TYPE_ACTIVE;
        }
        if (line_index < 243) {
            return CRT_TIMING_LINE_TYPE_BLANK;
        }
        if (line_index < 249) {
            return CRT_TIMING_LINE_TYPE_VSYNC;
        }
        return CRT_TIMING_LINE_TYPE_BLANK;
    case CRT_VIDEO_STANDARD_PAL:
        if (line_index >= 32 && line_index < (32 + 240)) {
            return CRT_TIMING_LINE_TYPE_ACTIVE;
        }
        if (line_index >= 304 && line_index < 312) {
            return CRT_TIMING_LINE_TYPE_VSYNC;
        }
        return CRT_TIMING_LINE_TYPE_BLANK;
    default:
        return CRT_TIMING_LINE_TYPE_BLANK;
    }
}
