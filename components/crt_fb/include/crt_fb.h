#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CRT_FB_FORMAT_INDEXED8 = 0,
} crt_fb_format_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    crt_fb_format_t format;
    void *buffer;
    size_t buffer_size;
} crt_fb_surface_t;

esp_err_t crt_fb_surface_init(crt_fb_surface_t *surface, uint16_t width, uint16_t height, crt_fb_format_t format);
esp_err_t crt_fb_surface_deinit(crt_fb_surface_t *surface);

#ifdef __cplusplus
}
#endif
