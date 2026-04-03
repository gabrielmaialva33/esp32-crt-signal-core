#include <stdbool.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "crt_core.h"

static const char *TAG = "app_main";
static const bool k_enable_color = CONFIG_CRT_ENABLE_COLOR;

static esp_err_t app_start_core(crt_video_standard_t video_standard)
{
    crt_core_config_t config = {
        .video_standard = video_standard,
        .enable_color = k_enable_color,
        .demo_pattern_mode = k_enable_color ? CRT_DEMO_PATTERN_COLOR_BARS_RAMP : CRT_DEMO_PATTERN_LUMA_BARS,
        .target_ready_depth = 8,
        .min_ready_depth = 4,
        .prep_task_core = 1,
    };

    esp_err_t err = crt_core_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "crt_core_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = crt_core_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "crt_core_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "ESP32 CRT signal core started: standard=%s color=%s pattern=%s",
             (video_standard == CRT_VIDEO_STANDARD_PAL) ? "PAL" : "NTSC",
             k_enable_color ? "on" : "off",
             (config.demo_pattern_mode == CRT_DEMO_PATTERN_COLOR_BARS_RAMP) ? "color_bars_ramp" : "luma_bars");

    return ESP_OK;
}

void app_main(void)
{
    crt_video_standard_t video_standard =
#if CONFIG_CRT_VIDEO_STANDARD_PAL
        CRT_VIDEO_STANDARD_PAL;
#else
        CRT_VIDEO_STANDARD_NTSC;
#endif

    if (app_start_core(video_standard) != ESP_OK) {
        return;
    }

#if CONFIG_CRT_TEST_STANDARD_TOGGLE
    while (true) {
        esp_err_t err;

        vTaskDelay(pdMS_TO_TICKS((uint32_t)CONFIG_CRT_TEST_STANDARD_TOGGLE_INTERVAL_S * 1000U));
        video_standard = (video_standard == CRT_VIDEO_STANDARD_PAL) ? CRT_VIDEO_STANDARD_NTSC : CRT_VIDEO_STANDARD_PAL;

        ESP_LOGI(TAG,
                 "test toggle: switching standard to %s (interval=%" PRIu32 "s)",
                 (video_standard == CRT_VIDEO_STANDARD_PAL) ? "PAL" : "NTSC",
                 (uint32_t)CONFIG_CRT_TEST_STANDARD_TOGGLE_INTERVAL_S);

        err = crt_core_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "crt_core_stop failed during toggle: %s", esp_err_to_name(err));
            break;
        }
        err = crt_core_deinit();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "crt_core_deinit failed during toggle: %s", esp_err_to_name(err));
            break;
        }
        if (app_start_core(video_standard) != ESP_OK) {
            break;
        }
    }
#endif
}
