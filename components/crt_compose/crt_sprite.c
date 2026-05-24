#include "crt_sprite.h"

#include "esp_attr.h"
#include "esp_check.h"

#include <string.h>

static uint8_t crt_sprite_size_px(crt_sprite_size_t size)
{
    switch (size) {
    case CRT_SPRITE_SIZE_16X16:
        return 16;
    case CRT_SPRITE_SIZE_32X32:
        return 32;
    case CRT_SPRITE_SIZE_8X8:
    default:
        return 8;
    }
}

static bool crt_sprite_size_valid(crt_sprite_size_t size)
{
    return size == CRT_SPRITE_SIZE_8X8 || size == CRT_SPRITE_SIZE_16X16 ||
           size == CRT_SPRITE_SIZE_32X32;
}

static bool crt_sprite_id_valid(const crt_sprite_layer_t *layer, uint8_t sprite_id)
{
    return layer != NULL && sprite_id < layer->sprite_count;
}

static uint8_t crt_sprite_compose_attr(uint8_t attr)
{
    return CRT_COMPOSE_PIXEL_SPRITE_OPAQUE |
           (((attr & CRT_SPRITE_ATTR_BG_PRIORITY) != 0u) ? CRT_COMPOSE_PIXEL_SPRITE_BG_PRIO : 0u) |
           (attr & CRT_SPRITE_ATTR_PALETTE_MASK);
}

esp_err_t crt_sprite_atlas_init(crt_sprite_atlas_t *atlas, const uint8_t *pixels, uint16_t width,
                                uint16_t height, uint16_t stride)
{
    ESP_RETURN_ON_FALSE(atlas != NULL, ESP_ERR_INVALID_ARG, "crt_sprite", "null atlas");
    ESP_RETURN_ON_FALSE(pixels != NULL, ESP_ERR_INVALID_ARG, "crt_sprite", "null pixels");
    ESP_RETURN_ON_FALSE(width > 0 && height > 0 && stride >= width, ESP_ERR_INVALID_ARG,
                        "crt_sprite", "invalid dimensions");
    ESP_RETURN_ON_FALSE((width % CRT_SPRITE_CELL_SIZE) == 0 && (height % CRT_SPRITE_CELL_SIZE) == 0,
                        ESP_ERR_INVALID_ARG, "crt_sprite", "atlas must use 8x8 cells");

    *atlas = (crt_sprite_atlas_t){
        .pixels = pixels,
        .width = width,
        .height = height,
        .stride = stride,
    };
    return ESP_OK;
}

esp_err_t crt_sprite_layer_init(crt_sprite_layer_t *layer, const crt_sprite_atlas_t *atlas,
                                uint8_t transparent_idx)
{
    ESP_RETURN_ON_FALSE(layer != NULL, ESP_ERR_INVALID_ARG, "crt_sprite", "null layer");
    ESP_RETURN_ON_FALSE(atlas != NULL && atlas->pixels != NULL, ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid atlas");

    memset(layer, 0, sizeof(*layer));
    layer->atlas = *atlas;
    layer->max_sprites_per_line = CRT_SPRITE_DEFAULT_PERLINE;
    layer->x_scale = 1;
    layer->transparent_idx = transparent_idx;
    return ESP_OK;
}

void crt_sprite_layer_set_max_sprites_per_line(crt_sprite_layer_t *layer, uint8_t max_sprites)
{
    if (layer != NULL) {
        layer->max_sprites_per_line = max_sprites;
    }
}

void crt_sprite_layer_set_x_scale(crt_sprite_layer_t *layer, uint8_t x_scale)
{
    (void)x_scale;
    if (layer != NULL) {
        layer->x_scale = 1;
    }
}

void crt_sprite_layer_reset_stats(crt_sprite_layer_t *layer)
{
    if (layer != NULL) {
        layer->overflow_count = 0;
        layer->last_line_considered = 0;
        layer->last_line_rendered = 0;
        layer->last_line_overflow = 0;
    }
}

esp_err_t crt_sprite_add(crt_sprite_layer_t *layer, uint16_t cell_x, uint16_t cell_y,
                         crt_sprite_size_t size, int16_t x, int16_t y, uint8_t *out_sprite_id)
{
    if (out_sprite_id != NULL) {
        *out_sprite_id = CRT_SPRITE_INVALID_ID;
    }

    ESP_RETURN_ON_FALSE(layer != NULL, ESP_ERR_INVALID_ARG, "crt_sprite", "null layer");
    ESP_RETURN_ON_FALSE(crt_sprite_size_valid(size), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid size");
    ESP_RETURN_ON_FALSE(layer->sprite_count < CRT_SPRITE_MAX_SPRITES, ESP_ERR_NO_MEM, "crt_sprite",
                        "sprite OAM full");

    const uint8_t sprite_px = crt_sprite_size_px(size);
    const uint32_t src_x = (uint32_t)cell_x * CRT_SPRITE_CELL_SIZE;
    const uint32_t src_y = (uint32_t)cell_y * CRT_SPRITE_CELL_SIZE;
    ESP_RETURN_ON_FALSE(src_x + sprite_px <= layer->atlas.width &&
                            src_y + sprite_px <= layer->atlas.height,
                        ESP_ERR_INVALID_ARG, "crt_sprite", "sprite outside atlas");

    const uint8_t sprite_id = layer->sprite_count;
    layer->sprites[sprite_id] = (crt_sprite_t){
        .x = x,
        .y = y,
        .cell_x = cell_x,
        .cell_y = cell_y,
        .size = size,
        .enabled = true,
        .attr = 0,
    };
    layer->sprite_count++;
    if (out_sprite_id != NULL) {
        *out_sprite_id = sprite_id;
    }
    return ESP_OK;
}

esp_err_t crt_sprite_set_enabled(crt_sprite_layer_t *layer, uint8_t sprite_id, bool enabled)
{
    ESP_RETURN_ON_FALSE(crt_sprite_id_valid(layer, sprite_id), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid sprite");
    layer->sprites[sprite_id].enabled = enabled;
    return ESP_OK;
}

esp_err_t crt_sprite_set_position(crt_sprite_layer_t *layer, uint8_t sprite_id, int16_t x,
                                  int16_t y)
{
    ESP_RETURN_ON_FALSE(crt_sprite_id_valid(layer, sprite_id), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid sprite");
    layer->sprites[sprite_id].x = x;
    layer->sprites[sprite_id].y = y;
    return ESP_OK;
}

esp_err_t crt_sprite_move_by(crt_sprite_layer_t *layer, uint8_t sprite_id, int16_t dx, int16_t dy)
{
    ESP_RETURN_ON_FALSE(crt_sprite_id_valid(layer, sprite_id), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid sprite");
    layer->sprites[sprite_id].x += dx;
    layer->sprites[sprite_id].y += dy;
    return ESP_OK;
}

esp_err_t crt_sprite_set_atlas_cell(crt_sprite_layer_t *layer, uint8_t sprite_id, uint16_t cell_x,
                                    uint16_t cell_y)
{
    ESP_RETURN_ON_FALSE(crt_sprite_id_valid(layer, sprite_id), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid sprite");

    crt_sprite_t *sprite = &layer->sprites[sprite_id];
    const uint8_t sprite_px = crt_sprite_size_px(sprite->size);
    const uint32_t src_x = (uint32_t)cell_x * CRT_SPRITE_CELL_SIZE;
    const uint32_t src_y = (uint32_t)cell_y * CRT_SPRITE_CELL_SIZE;
    ESP_RETURN_ON_FALSE(src_x + sprite_px <= layer->atlas.width &&
                            src_y + sprite_px <= layer->atlas.height,
                        ESP_ERR_INVALID_ARG, "crt_sprite", "sprite outside atlas");

    sprite->cell_x = cell_x;
    sprite->cell_y = cell_y;
    return ESP_OK;
}

esp_err_t crt_sprite_set_frame(crt_sprite_layer_t *layer, uint8_t sprite_id, uint16_t frame)
{
    ESP_RETURN_ON_FALSE(crt_sprite_id_valid(layer, sprite_id), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid sprite");
    return crt_sprite_set_atlas_cell(layer, sprite_id, frame, layer->sprites[sprite_id].cell_y);
}

esp_err_t crt_sprite_set_size(crt_sprite_layer_t *layer, uint8_t sprite_id, crt_sprite_size_t size)
{
    ESP_RETURN_ON_FALSE(crt_sprite_id_valid(layer, sprite_id), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid sprite");
    ESP_RETURN_ON_FALSE(crt_sprite_size_valid(size), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid size");

    crt_sprite_t *sprite = &layer->sprites[sprite_id];
    const uint8_t sprite_px = crt_sprite_size_px(size);
    const uint32_t src_x = (uint32_t)sprite->cell_x * CRT_SPRITE_CELL_SIZE;
    const uint32_t src_y = (uint32_t)sprite->cell_y * CRT_SPRITE_CELL_SIZE;
    ESP_RETURN_ON_FALSE(src_x + sprite_px <= layer->atlas.width &&
                            src_y + sprite_px <= layer->atlas.height,
                        ESP_ERR_INVALID_ARG, "crt_sprite", "sprite outside atlas");

    sprite->size = size;
    return ESP_OK;
}

esp_err_t crt_sprite_set_attr(crt_sprite_layer_t *layer, uint8_t sprite_id, uint8_t attr)
{
    ESP_RETURN_ON_FALSE(crt_sprite_id_valid(layer, sprite_id), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid sprite");
    layer->sprites[sprite_id].attr = attr;
    return ESP_OK;
}

uint8_t crt_sprite_get_attr(const crt_sprite_layer_t *layer, uint8_t sprite_id)
{
    if (!crt_sprite_id_valid(layer, sprite_id)) {
        return 0;
    }
    return layer->sprites[sprite_id].attr;
}

esp_err_t crt_sprite_get(const crt_sprite_layer_t *layer, uint8_t sprite_id,
                         crt_sprite_t *out_sprite)
{
    ESP_RETURN_ON_FALSE(crt_sprite_id_valid(layer, sprite_id), ESP_ERR_INVALID_ARG, "crt_sprite",
                        "invalid sprite");
    ESP_RETURN_ON_FALSE(out_sprite != NULL, ESP_ERR_INVALID_ARG, "crt_sprite", "null out");
    *out_sprite = layer->sprites[sprite_id];
    return ESP_OK;
}

IRAM_ATTR bool crt_sprite_layer_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                      uint16_t width)
{
    return crt_sprite_layer_fetch_with_attrs(ctx, logical_line, idx_out, NULL, width);
}

IRAM_ATTR bool crt_sprite_layer_fetch_with_attrs(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                                 uint8_t *attr_out, uint16_t width)
{
    crt_sprite_layer_t *layer = (crt_sprite_layer_t *)ctx;
    if (layer == NULL || idx_out == NULL || width == 0 || layer->atlas.pixels == NULL) {
        return false;
    }

    /* Pre-scan: bail out fast on lines no sprite covers. Skipping the
     * full-line memset here lets compose delegate the line back to the
     * fused base override (zero extra cost). */
    bool any_y_match = false;
    for (uint8_t i = 0; i < layer->sprite_count; ++i) {
        const crt_sprite_t *sprite = &layer->sprites[i];
        if (!sprite->enabled) {
            continue;
        }
        const int32_t rel_y = (int32_t)logical_line - sprite->y;
        const uint8_t sprite_px = crt_sprite_size_px(sprite->size);
        if (rel_y >= 0 && rel_y < sprite_px) {
            any_y_match = true;
            break;
        }
    }
    if (!any_y_match) {
        layer->last_line_considered = 0;
        layer->last_line_rendered = 0;
        layer->last_line_overflow = 0;
        return false;
    }

    memset(idx_out, layer->transparent_idx, width);
    if (attr_out != NULL) {
        memset(attr_out, 0, width);
    }
    layer->last_line_considered = 0;
    layer->last_line_rendered = 0;
    layer->last_line_overflow = 0;

    for (uint8_t i = 0; i < layer->sprite_count; ++i) {
        const crt_sprite_t *sprite = &layer->sprites[i];
        if (!sprite->enabled) {
            continue;
        }

        const uint8_t sprite_px = crt_sprite_size_px(sprite->size);
        const int32_t rel_y = (int32_t)logical_line - sprite->y;
        if (rel_y < 0 || rel_y >= sprite_px) {
            continue;
        }

        layer->last_line_considered++;
        if (layer->last_line_rendered >= layer->max_sprites_per_line) {
            layer->last_line_overflow++;
            continue;
        }

        const uint8_t attr = sprite->attr;
        const uint8_t compose_attr = crt_sprite_compose_attr(attr);
        const uint32_t sample_y = ((attr & CRT_SPRITE_ATTR_VFLIP) != 0u)
                                      ? (uint32_t)(sprite_px - 1u - (uint8_t)rel_y)
                                      : (uint32_t)rel_y;
        const uint32_t src_y = ((uint32_t)sprite->cell_y * CRT_SPRITE_CELL_SIZE) + sample_y;
        const uint32_t src_x0 = (uint32_t)sprite->cell_x * CRT_SPRITE_CELL_SIZE;
        const uint8_t *src = &layer->atlas.pixels[(src_y * layer->atlas.stride) + src_x0];

        bool wrote_pixel = false;
        if ((attr & CRT_SPRITE_ATTR_HFLIP) != 0u) {
            for (uint8_t sx = 0; sx < sprite_px; ++sx) {
                const uint8_t sample = src[sprite_px - 1u - sx];
                if (sample == layer->transparent_idx) {
                    continue;
                }

                const int32_t logical_x = (int32_t)sprite->x + sx;
                if (logical_x >= 0 && logical_x < width) {
                    idx_out[logical_x] = sample;
                    if (attr_out != NULL) {
                        attr_out[logical_x] = compose_attr;
                    }
                    wrote_pixel = true;
                }
            }
        } else {
            for (uint8_t sx = 0; sx < sprite_px; ++sx) {
                const uint8_t sample = src[sx];
                if (sample == layer->transparent_idx) {
                    continue;
                }

                const int32_t logical_x = (int32_t)sprite->x + sx;
                if (logical_x >= 0 && logical_x < width) {
                    idx_out[logical_x] = sample;
                    if (attr_out != NULL) {
                        attr_out[logical_x] = compose_attr;
                    }
                    wrote_pixel = true;
                }
            }
        }

        if (wrote_pixel) {
            layer->last_line_rendered++;
        }
    }

    if (layer->last_line_overflow > 0) {
        layer->overflow_count += layer->last_line_overflow;
    }
    return layer->last_line_rendered > 0;
}

IRAM_ATTR uint8_t crt_sprite_layer_collect_scanline(crt_sprite_layer_t *layer,
                                                    uint16_t logical_line, uint16_t width,
                                                    crt_sprite_scanline_span_t *out_spans,
                                                    uint8_t span_capacity)
{
    if (layer == NULL || out_spans == NULL || width == 0 || span_capacity == 0 ||
        layer->atlas.pixels == NULL) {
        return 0;
    }

    layer->last_line_considered = 0;
    layer->last_line_rendered = 0;
    layer->last_line_overflow = 0;

    const uint8_t max_spans =
        (layer->max_sprites_per_line < span_capacity) ? layer->max_sprites_per_line : span_capacity;
    uint8_t span_count = 0;

    for (uint8_t i = 0; i < layer->sprite_count; ++i) {
        const crt_sprite_t *sprite = &layer->sprites[i];
        if (!sprite->enabled) {
            continue;
        }

        const uint8_t sprite_px = crt_sprite_size_px(sprite->size);
        const int32_t rel_y = (int32_t)logical_line - sprite->y;
        if (rel_y < 0 || rel_y >= sprite_px) {
            continue;
        }

        layer->last_line_considered++;

        const int32_t x0 = sprite->x;
        const int32_t x1 = (int32_t)sprite->x + sprite_px;
        if (x1 <= 0 || x0 >= width) {
            continue;
        }

        if (span_count >= max_spans) {
            layer->last_line_overflow++;
            continue;
        }

        const uint8_t attr = sprite->attr;
        const uint32_t sample_y = ((attr & CRT_SPRITE_ATTR_VFLIP) != 0u)
                                      ? (uint32_t)(sprite_px - 1u - (uint8_t)rel_y)
                                      : (uint32_t)rel_y;
        const uint32_t src_y = ((uint32_t)sprite->cell_y * CRT_SPRITE_CELL_SIZE) + sample_y;
        const uint32_t src_x0 = (uint32_t)sprite->cell_x * CRT_SPRITE_CELL_SIZE;
        const uint8_t *src = &layer->atlas.pixels[(src_y * layer->atlas.stride) + src_x0];

        const uint16_t dst_x = (x0 < 0) ? 0u : (uint16_t)x0;
        const uint16_t dst_end = (x1 > width) ? width : (uint16_t)x1;
        const uint8_t src_start = (uint8_t)(dst_x - x0);
        const bool hflip = (attr & CRT_SPRITE_ATTR_HFLIP) != 0u;
        const uint8_t *span_src = hflip ? &src[sprite_px - 1u - src_start] : &src[src_start];
        const int8_t span_step = hflip ? -1 : 1;

        bool wrote_pixel = false;
        const uint8_t *sample_src = span_src;
        for (uint8_t sx = 0; sx < (uint8_t)(dst_end - dst_x); ++sx) {
            if (*sample_src != layer->transparent_idx) {
                wrote_pixel = true;
                break;
            }
            sample_src += span_step;
        }
        if (!wrote_pixel) {
            continue;
        }

        out_spans[span_count++] = (crt_sprite_scanline_span_t){
            .dst_x = dst_x,
            .width = (uint8_t)(dst_end - dst_x),
            .src = span_src,
            .src_step = span_step,
            .attr = crt_sprite_compose_attr(attr),
        };
        layer->last_line_rendered++;
    }

    if (layer->last_line_overflow > 0) {
        layer->overflow_count += layer->last_line_overflow;
    }
    return span_count;
}
