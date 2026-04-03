#include "crt_fb.h"

#include <string.h>

#include "esp_check.h"

esp_err_t crt_fb_surface_init(crt_fb_surface_t *surface, uint16_t width, uint16_t height, crt_fb_format_t format)
{
    ESP_RETURN_ON_FALSE(surface != NULL, ESP_ERR_INVALID_ARG, "crt_fb", "surface is null");

    *surface = (crt_fb_surface_t) {
        .width = width,
        .height = height,
        .format = format,
        .buffer = NULL,
        .buffer_size = 0,
    };
    return ESP_OK;
}

esp_err_t crt_fb_surface_deinit(crt_fb_surface_t *surface)
{
    ESP_RETURN_ON_FALSE(surface != NULL, ESP_ERR_INVALID_ARG, "crt_fb", "surface is null");

    memset(surface, 0, sizeof(*surface));
    return ESP_OK;
}
