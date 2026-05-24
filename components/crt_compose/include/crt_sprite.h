#ifndef CRT_SPRITE_H
#define CRT_SPRITE_H

#include "crt_compose.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CRT_SPRITE_MAX_SPRITES
#define CRT_SPRITE_MAX_SPRITES 64
#endif

#define CRT_SPRITE_CELL_SIZE       8U
#define CRT_SPRITE_INVALID_ID      ((uint8_t)0xFFu)
#define CRT_SPRITE_DEFAULT_PERLINE 8U

#define CRT_SPRITE_ATTR_HFLIP       (1u << 0) /* mirror columns */
#define CRT_SPRITE_ATTR_VFLIP       (1u << 1) /* mirror rows */
#define CRT_SPRITE_ATTR_BG_PRIORITY (1u << 2) /* sprite drawn behind BG */
/* Bits 3..6 select a crt_compose palette bank; bit 7 remains reserved. */
#define CRT_SPRITE_ATTR_PALETTE_SHIFT 3
#define CRT_SPRITE_ATTR_PALETTE_MASK  (0x0Fu << CRT_SPRITE_ATTR_PALETTE_SHIFT)

typedef enum {
    CRT_SPRITE_SIZE_8X8 = 0,
    CRT_SPRITE_SIZE_16X16,
    CRT_SPRITE_SIZE_32X32,
} crt_sprite_size_t;

typedef struct {
    const uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
} crt_sprite_atlas_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t cell_x;
    uint16_t cell_y;
    crt_sprite_size_t size;
    bool enabled;
    uint8_t attr;
} crt_sprite_t;

typedef struct {
    crt_sprite_atlas_t atlas;
    crt_sprite_t sprites[CRT_SPRITE_MAX_SPRITES];
    uint8_t sprite_count;
    uint8_t max_sprites_per_line;
    /** Deprecated compatibility field. Sprite fetch always uses logical x_scale=1. */
    uint8_t x_scale;
    uint8_t transparent_idx;
    uint32_t overflow_count;
    uint8_t last_line_considered;
    uint8_t last_line_rendered;
    uint8_t last_line_overflow;
} crt_sprite_layer_t;

typedef struct {
    uint16_t dst_x;
    uint8_t width;
    const uint8_t *src;
    int8_t src_step;
    uint8_t attr;
} crt_sprite_scanline_span_t;

esp_err_t crt_sprite_atlas_init(crt_sprite_atlas_t *atlas, const uint8_t *pixels, uint16_t width,
                                uint16_t height, uint16_t stride);

esp_err_t crt_sprite_layer_init(crt_sprite_layer_t *layer, const crt_sprite_atlas_t *atlas,
                                uint8_t transparent_idx);

void crt_sprite_layer_set_max_sprites_per_line(crt_sprite_layer_t *layer, uint8_t max_sprites);

/** Deprecated compatibility no-op. Sprites always render in 256-pixel logical space. */
void crt_sprite_layer_set_x_scale(crt_sprite_layer_t *layer, uint8_t x_scale);

void crt_sprite_layer_reset_stats(crt_sprite_layer_t *layer);

uint32_t crt_sprite_layer_get_overflow_count(const crt_sprite_layer_t *layer);

uint8_t crt_sprite_layer_get_last_line_overflow(const crt_sprite_layer_t *layer);

esp_err_t crt_sprite_add(crt_sprite_layer_t *layer, uint16_t cell_x, uint16_t cell_y,
                         crt_sprite_size_t size, int16_t x, int16_t y, uint8_t *out_sprite_id);

esp_err_t crt_sprite_set_enabled(crt_sprite_layer_t *layer, uint8_t sprite_id, bool enabled);

esp_err_t crt_sprite_set_position(crt_sprite_layer_t *layer, uint8_t sprite_id, int16_t x,
                                  int16_t y);

esp_err_t crt_sprite_move_by(crt_sprite_layer_t *layer, uint8_t sprite_id, int16_t dx, int16_t dy);

esp_err_t crt_sprite_set_atlas_cell(crt_sprite_layer_t *layer, uint8_t sprite_id, uint16_t cell_x,
                                    uint16_t cell_y);

esp_err_t crt_sprite_set_frame(crt_sprite_layer_t *layer, uint8_t sprite_id, uint16_t frame);

esp_err_t crt_sprite_set_size(crt_sprite_layer_t *layer, uint8_t sprite_id, crt_sprite_size_t size);

/**
 * @brief Set one OAM attribute byte for a sprite.
 *
 * Returns ESP_ERR_INVALID_ARG for a NULL layer or invalid sprite_id.
 */
esp_err_t crt_sprite_set_attr(crt_sprite_layer_t *layer, uint8_t sprite_id, uint8_t attr);

/**
 * @brief Read one OAM attribute byte. Returns 0 for a NULL layer or invalid sprite_id.
 */
uint8_t crt_sprite_get_attr(const crt_sprite_layer_t *layer, uint8_t sprite_id);

esp_err_t crt_sprite_get(const crt_sprite_layer_t *layer, uint8_t sprite_id,
                         crt_sprite_t *out_sprite);

/**
 * @brief Fetch one indexed sprite scanline, applying per-sprite H/V flip attributes.
 */
bool crt_sprite_layer_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out, uint16_t width);

/**
 * @brief Attribute-aware compose fetch adapter. Marks opaque sprite pixels and
 *        CRT_SPRITE_ATTR_BG_PRIORITY requests in attr_out.
 */
bool crt_sprite_layer_fetch_with_attrs(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                       uint8_t *attr_out, uint16_t width);

/**
 * @brief Collect clipped visible sprite spans for one logical scanline.
 *
 * This is the low-overhead adapter for fused compose paths: it returns source
 * row spans without clearing or materializing a full 256-byte sprite line.
 */
uint8_t crt_sprite_layer_collect_scanline(crt_sprite_layer_t *layer, uint16_t logical_line,
                                          uint16_t width, crt_sprite_scanline_span_t *out_spans,
                                          uint8_t span_capacity);

#ifdef __cplusplus
}
#endif

#endif /* CRT_SPRITE_H */
