#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "crt_scanline.h"

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
    uint8_t *buffer;
    size_t buffer_size;
    uint16_t *palette;
    uint16_t palette_size;
} crt_fb_surface_t;

/* ── Lifecycle ────────────────────────────────────────────────────── */

esp_err_t crt_fb_surface_init(crt_fb_surface_t *surface, uint16_t width, uint16_t height, crt_fb_format_t format);
esp_err_t crt_fb_surface_alloc(crt_fb_surface_t *surface);
esp_err_t crt_fb_surface_free(crt_fb_surface_t *surface);
esp_err_t crt_fb_surface_deinit(crt_fb_surface_t *surface);

/* ── Pixel access ─────────────────────────────────────────────────── */

uint8_t *crt_fb_row(const crt_fb_surface_t *surface, uint16_t y);
void     crt_fb_put(crt_fb_surface_t *surface, uint16_t x, uint16_t y, uint8_t value);
uint8_t  crt_fb_get(const crt_fb_surface_t *surface, uint16_t x, uint16_t y);
void     crt_fb_clear(crt_fb_surface_t *surface, uint8_t value);

/* ── Palette ──────────────────────────────────────────────────────── */

void crt_fb_palette_set(crt_fb_surface_t *surface, uint8_t index, uint16_t dac_level);
void crt_fb_palette_init_grayscale(crt_fb_surface_t *surface,
                                   uint16_t blank_level,
                                   uint16_t white_level);

/* ── Scanline hook ────────────────────────────────────────────────── */

/**
 * @brief Built-in scanline hook that renders from framebuffer.
 *
 * Reads pixel row for the current logical line, maps through palette LUT,
 * and writes DAC samples to active_buf. Nearest-neighbor scaling from
 * fb width to active_width.
 *
 * Register with: crt_register_scanline_hook(crt_fb_scanline_hook, &surface);
 */
void crt_fb_scanline_hook(const crt_scanline_t *scanline,
                          uint16_t *active_buf,
                          uint16_t active_width,
                          void *user_data);

#ifdef __cplusplus
}
#endif
