#include "esp_err.h"
#include "esp_log.h"

#include "crt_core.h"

static const char *TAG = "app_main";

void app_main(void)
{
    crt_core_config_t config = {
        .video_standard = CRT_VIDEO_STANDARD_NTSC,
        .demo_pattern_mode = CRT_DEMO_PATTERN_COLOR_BARS_RAMP,
        .target_ready_depth = 4,
        .min_ready_depth = 2,
        .prep_task_core = 1,
    };

    esp_err_t err = crt_core_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "crt_core_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = crt_core_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "crt_core_start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "ESP32 CRT signal core base composite pipeline started");
}
