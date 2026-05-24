#include "crt_ppu.h"

#include "esp_check.h"

#include <string.h>

esp_err_t crt_ppu_init(crt_ppu_t *ppu, const crt_ppu_config_t *config)
{
    ESP_RETURN_ON_FALSE(ppu != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null ppu");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null config");
    ESP_RETURN_ON_FALSE(config->sprite_atlas != NULL, ESP_ERR_INVALID_ARG, "crt_ppu",
                        "null sprite atlas");

    memset(ppu, 0, sizeof(*ppu));
    ppu->tile_layer_id = CRT_COMPOSE_LAYER_INVALID;
    ppu->sprite_layer_id = CRT_COMPOSE_LAYER_INVALID;

    esp_err_t err = crt_tile_init(&ppu->tile, config->visible_w_tiles, config->visible_h_tiles,
                                  config->pitch_w_tiles, config->pitch_h_tiles,
                                  config->pattern_table, config->pattern_count, config->nametable);
    if (err != ESP_OK) {
        return err;
    }
    crt_tile_set_attributes(&ppu->tile, config->attributes);
    crt_tile_set_palette(&ppu->tile, config->palette);
    crt_tile_set_palette_banks(&ppu->tile, config->palette_banks);

    err =
        crt_sprite_layer_init(&ppu->sprites, config->sprite_atlas, config->sprite_transparent_idx);
    if (err != ESP_OK) {
        return err;
    }
    if (config->max_sprites_per_line != 0u) {
        crt_sprite_layer_set_max_sprites_per_line(&ppu->sprites, config->max_sprites_per_line);
    }

    err = crt_compose_init(&ppu->compose);
    if (err != ESP_OK) {
        return err;
    }
    err = crt_compose_set_palette(&ppu->compose, config->palette, config->palette_size);
    if (err != ESP_OK) {
        return err;
    }
    crt_compose_set_palette_banks(&ppu->compose, config->palette_banks);

    err = crt_compose_add_layer_fused_with_attrs_with_id(
        &ppu->compose, crt_tile_layer_fetch_with_attrs, crt_tile_scanline_hook, &ppu->tile,
        &ppu->tile_layer_id);
    if (err != ESP_OK) {
        return err;
    }
    return crt_compose_add_layer_with_attrs_with_id(
        &ppu->compose, crt_sprite_layer_fetch_with_attrs, &ppu->sprites,
        config->sprite_transparent_idx, &ppu->sprite_layer_id);
}

void crt_ppu_set_scroll(crt_ppu_t *ppu, int x_px, int y_px)
{
    if (ppu != NULL) {
        crt_tile_set_scroll(&ppu->tile, x_px, y_px);
    }
}

void crt_ppu_set_tile(crt_ppu_t *ppu, uint16_t col, uint16_t row, uint8_t tile_idx)
{
    if (ppu != NULL) {
        crt_tile_set_tile(&ppu->tile, col, row, tile_idx);
    }
}

uint8_t crt_ppu_get_tile(const crt_ppu_t *ppu, uint16_t col, uint16_t row)
{
    return (ppu != NULL) ? crt_tile_get_tile(&ppu->tile, col, row) : 0;
}

void crt_ppu_set_attr(crt_ppu_t *ppu, uint16_t col, uint16_t row, uint8_t attr)
{
    if (ppu != NULL) {
        crt_tile_set_attr(&ppu->tile, col, row, attr);
    }
}

uint8_t crt_ppu_get_attr(const crt_ppu_t *ppu, uint16_t col, uint16_t row)
{
    return (ppu != NULL) ? crt_tile_get_attr(&ppu->tile, col, row) : 0;
}

esp_err_t crt_ppu_load_nes_attributes(crt_ppu_t *ppu, uint8_t *dst_attrs, const uint8_t *nes_attrs,
                                      uint16_t nes_pitch_bytes, uint8_t preserve_mask)
{
    ESP_RETURN_ON_FALSE(ppu != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null ppu");
    ESP_RETURN_ON_FALSE(dst_attrs != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null dst attrs");
    ESP_RETURN_ON_FALSE(nes_attrs != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null nes attrs");
    ESP_RETURN_ON_FALSE(nes_pitch_bytes != 0u, ESP_ERR_INVALID_ARG, "crt_ppu", "zero nes pitch");

    const uint16_t visible_w = ppu->tile.visible_w_tiles;
    const uint16_t visible_h = ppu->tile.visible_h_tiles;
    const uint16_t dst_pitch = ppu->tile.pitch_w_tiles;
    const uint16_t required_pitch = (uint16_t)((visible_w + 3u) / 4u);
    ESP_RETURN_ON_FALSE(nes_pitch_bytes >= required_pitch, ESP_ERR_INVALID_ARG, "crt_ppu",
                        "short nes pitch");

    for (uint16_t row = 0; row < visible_h; ++row) {
        for (uint16_t col = 0; col < visible_w; ++col) {
            const uint16_t attr_row = (uint16_t)(row >> 2);
            const uint16_t attr_col = (uint16_t)(col >> 2);
            const uint8_t packed = nes_attrs[(size_t)attr_row * nes_pitch_bytes + attr_col];
            const uint8_t quadrant =
                (uint8_t)(((row & 0x02u) != 0u ? 2u : 0u) | ((col & 0x02u) != 0u ? 1u : 0u));
            const uint8_t bank = (uint8_t)((packed >> (quadrant * 2u)) & 0x03u);
            uint8_t *dst = &dst_attrs[(size_t)row * dst_pitch + col];
            *dst = (uint8_t)((*dst & preserve_mask) | (bank << CRT_TILE_ATTR_PALETTE_SHIFT));
        }
    }
    crt_tile_set_attributes(&ppu->tile, dst_attrs);
    return ESP_OK;
}

esp_err_t crt_ppu_add_sprite(crt_ppu_t *ppu, uint16_t cell_x, uint16_t cell_y,
                             crt_sprite_size_t size, int16_t x, int16_t y, uint8_t attr,
                             uint8_t *out_sprite_id)
{
    ESP_RETURN_ON_FALSE(ppu != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null ppu");
    uint8_t sprite_id = CRT_SPRITE_INVALID_ID;
    esp_err_t err = crt_sprite_add(&ppu->sprites, cell_x, cell_y, size, x, y, &sprite_id);
    if (err != ESP_OK) {
        return err;
    }
    err = crt_sprite_set_attr(&ppu->sprites, sprite_id, attr);
    if (out_sprite_id != NULL) {
        *out_sprite_id = sprite_id;
    }
    return err;
}

esp_err_t crt_ppu_set_sprite_attr(crt_ppu_t *ppu, uint8_t sprite_id, uint8_t attr)
{
    ESP_RETURN_ON_FALSE(ppu != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null ppu");
    return crt_sprite_set_attr(&ppu->sprites, sprite_id, attr);
}

esp_err_t crt_ppu_set_sprite_position(crt_ppu_t *ppu, uint8_t sprite_id, int16_t x, int16_t y)
{
    ESP_RETURN_ON_FALSE(ppu != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null ppu");
    return crt_sprite_set_position(&ppu->sprites, sprite_id, x, y);
}

esp_err_t crt_ppu_move_sprite_by(crt_ppu_t *ppu, uint8_t sprite_id, int16_t dx, int16_t dy)
{
    ESP_RETURN_ON_FALSE(ppu != NULL, ESP_ERR_INVALID_ARG, "crt_ppu", "null ppu");
    return crt_sprite_move_by(&ppu->sprites, sprite_id, dx, dy);
}

void crt_ppu_reset_sprite_stats(crt_ppu_t *ppu)
{
    if (ppu != NULL) {
        crt_sprite_layer_reset_stats(&ppu->sprites);
    }
}

uint32_t crt_ppu_get_sprite_overflow_count(const crt_ppu_t *ppu)
{
    return (ppu != NULL) ? crt_sprite_layer_get_overflow_count(&ppu->sprites) : 0;
}

uint8_t crt_ppu_get_sprite_last_line_overflow(const crt_ppu_t *ppu)
{
    return (ppu != NULL) ? crt_sprite_layer_get_last_line_overflow(&ppu->sprites) : 0;
}

uint8_t crt_ppu_get_sprite_max_line_considered(const crt_ppu_t *ppu)
{
    return (ppu != NULL) ? crt_sprite_layer_get_max_line_considered(&ppu->sprites) : 0;
}

uint8_t crt_ppu_get_sprite_max_line_rendered(const crt_ppu_t *ppu)
{
    return (ppu != NULL) ? crt_sprite_layer_get_max_line_rendered(&ppu->sprites) : 0;
}

void crt_ppu_reset_compose_stats(crt_ppu_t *ppu)
{
    if (ppu != NULL) {
        crt_compose_reset_stats(&ppu->compose);
    }
}

crt_compose_stats_t crt_ppu_get_compose_stats(const crt_ppu_t *ppu)
{
    return (ppu != NULL) ? crt_compose_get_stats(&ppu->compose) : (crt_compose_stats_t){0};
}

void crt_ppu_scanline_hook(const crt_scanline_t *scanline, uint16_t *active_buf,
                           uint16_t active_width, void *user_data)
{
    crt_ppu_t *ppu = (crt_ppu_t *)user_data;
    if (ppu != NULL) {
        crt_compose_scanline_hook(scanline, active_buf, active_width, &ppu->compose);
    }
}

void crt_ppu_scanline_hook_rgb332_256(const crt_scanline_t *scanline, uint16_t *active_buf,
                                      uint16_t active_width, void *user_data)
{
    crt_ppu_t *ppu = (crt_ppu_t *)user_data;
    if (ppu != NULL) {
        crt_compose_scanline_hook_rgb332_256(scanline, active_buf, active_width, &ppu->compose);
    }
}
