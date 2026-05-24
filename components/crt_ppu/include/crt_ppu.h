#ifndef CRT_PPU_H
#define CRT_PPU_H

#include "crt_compose.h"
#include "crt_scanline.h"
#include "crt_sprite.h"
#include "crt_tile.h"

#include "esp_err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRT_PPU_MAX_PENDING_UPDATES 64U

typedef enum {
    CRT_PPU_PENDING_TILE = 0,
    CRT_PPU_PENDING_ATTR,
    CRT_PPU_PENDING_SPRITE_POSITION,
    CRT_PPU_PENDING_SPRITE_ATTR,
} crt_ppu_pending_update_type_t;

typedef struct {
    crt_ppu_pending_update_type_t type;
    union {
        struct {
            uint16_t col;
            uint16_t row;
            uint8_t value;
        } cell;
        struct {
            uint8_t sprite_id;
            int16_t x;
            int16_t y;
        } sprite_position;
        struct {
            uint8_t sprite_id;
            uint8_t attr;
        } sprite_attr;
    } data;
} crt_ppu_pending_update_t;

typedef struct {
    uint16_t visible_w_tiles;
    uint16_t visible_h_tiles;
    uint16_t pitch_w_tiles;
    uint16_t pitch_h_tiles;
    const uint8_t *pattern_table;
    uint16_t pattern_count;
    uint8_t *nametable;
    const uint8_t *attributes;

    const crt_sprite_atlas_t *sprite_atlas;
    uint8_t sprite_transparent_idx;
    uint8_t max_sprites_per_line;

    const uint16_t *palette;
    uint16_t palette_size;
    const crt_compose_palette_banks_t *palette_banks;
} crt_ppu_config_t;

typedef struct {
    crt_compose_t compose;
    crt_tile_layer_t tile;
    crt_sprite_layer_t sprites;
    uint8_t tile_layer_id;
    uint8_t sprite_layer_id;
    crt_ppu_pending_update_t pending_updates[CRT_PPU_MAX_PENDING_UPDATES];
    uint8_t pending_update_count;
} crt_ppu_t;

esp_err_t crt_ppu_init(crt_ppu_t *ppu, const crt_ppu_config_t *config);

void crt_ppu_set_scroll(crt_ppu_t *ppu, int x_px, int y_px);

void crt_ppu_set_tile(crt_ppu_t *ppu, uint16_t col, uint16_t row, uint8_t tile_idx);

uint8_t crt_ppu_get_tile(const crt_ppu_t *ppu, uint16_t col, uint16_t row);

void crt_ppu_set_attr(crt_ppu_t *ppu, uint16_t col, uint16_t row, uint8_t attr);

uint8_t crt_ppu_get_attr(const crt_ppu_t *ppu, uint16_t col, uint16_t row);

/* Staged updates are applied only by crt_ppu_commit_frame. Use them from
 * producers outside the frame hook so visible scanlines see one stable PPU
 * state for the whole frame. The immediate setters above remain available for
 * compatibility and for code already running at a frame-safe point. */
esp_err_t crt_ppu_stage_tile(crt_ppu_t *ppu, uint16_t col, uint16_t row, uint8_t tile_idx);

esp_err_t crt_ppu_stage_attr(crt_ppu_t *ppu, uint16_t col, uint16_t row, uint8_t attr);

esp_err_t crt_ppu_stage_sprite_position(crt_ppu_t *ppu, uint8_t sprite_id, int16_t x, int16_t y);

esp_err_t crt_ppu_stage_sprite_attr(crt_ppu_t *ppu, uint8_t sprite_id, uint8_t attr);

uint8_t crt_ppu_get_pending_update_count(const crt_ppu_t *ppu);

esp_err_t crt_ppu_commit_frame(crt_ppu_t *ppu);

void crt_ppu_frame_hook(uint32_t frame_number, void *user_data);

esp_err_t crt_ppu_load_nes_attributes(crt_ppu_t *ppu, uint8_t *dst_attrs, const uint8_t *nes_attrs,
                                      uint16_t nes_pitch_bytes, uint8_t preserve_mask);

esp_err_t crt_ppu_add_sprite(crt_ppu_t *ppu, uint16_t cell_x, uint16_t cell_y,
                             crt_sprite_size_t size, int16_t x, int16_t y, uint8_t attr,
                             uint8_t *out_sprite_id);

esp_err_t crt_ppu_set_sprite_attr(crt_ppu_t *ppu, uint8_t sprite_id, uint8_t attr);

esp_err_t crt_ppu_set_sprite_position(crt_ppu_t *ppu, uint8_t sprite_id, int16_t x, int16_t y);

esp_err_t crt_ppu_move_sprite_by(crt_ppu_t *ppu, uint8_t sprite_id, int16_t dx, int16_t dy);

void crt_ppu_reset_sprite_stats(crt_ppu_t *ppu);

uint32_t crt_ppu_get_sprite_overflow_count(const crt_ppu_t *ppu);

uint8_t crt_ppu_get_sprite_last_line_overflow(const crt_ppu_t *ppu);

uint8_t crt_ppu_get_sprite_max_line_considered(const crt_ppu_t *ppu);

uint8_t crt_ppu_get_sprite_max_line_rendered(const crt_ppu_t *ppu);

void crt_ppu_reset_compose_stats(crt_ppu_t *ppu);

crt_compose_stats_t crt_ppu_get_compose_stats(const crt_ppu_t *ppu);

void crt_ppu_scanline_hook(const crt_scanline_t *scanline, uint16_t *active_buf,
                           uint16_t active_width, void *user_data);

void crt_ppu_scanline_hook_rgb332_256(const crt_scanline_t *scanline, uint16_t *active_buf,
                                      uint16_t active_width, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* CRT_PPU_H */
