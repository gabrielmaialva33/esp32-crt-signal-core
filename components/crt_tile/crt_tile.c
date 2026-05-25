#include "crt_tile.h"

#include "crt_compose.h"

#include "esp_attr.h"
#include "esp_check.h"

#include <string.h>

#define CRT_TILE_STACK_LOGICAL_W 256U

/* ── Internal helpers ─────────────────────────────────────────────── */

/* True when v is a positive power of two. */
static inline bool is_pow2(uint16_t v)
{
    return v != 0u && (v & (uint16_t)(v - 1u)) == 0u;
}

/* Wrap x into [0, mod). mod must be > 0. For power-of-two mod the
 * caller should use `& (mod - 1)` in the hot path instead. */
static inline uint16_t wrap_u16(int v, int mod)
{
    int r = v % mod;
    if (r < 0) {
        r += mod;
    }
    return (uint16_t)r;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

esp_err_t crt_tile_init(crt_tile_layer_t *t, uint16_t visible_w, uint16_t visible_h,
                        uint16_t pitch_w, uint16_t pitch_h, const uint8_t *pattern_table,
                        uint16_t pattern_count, uint8_t *nametable)
{
    ESP_RETURN_ON_FALSE(t != NULL, ESP_ERR_INVALID_ARG, "crt_tile", "null state");
    ESP_RETURN_ON_FALSE(visible_w > 0 && visible_h > 0, ESP_ERR_INVALID_ARG, "crt_tile",
                        "zero visible dim");
    ESP_RETURN_ON_FALSE(pitch_w >= visible_w && pitch_h >= visible_h, ESP_ERR_INVALID_ARG,
                        "crt_tile", "pitch smaller than visible");
    ESP_RETURN_ON_FALSE(pattern_table != NULL && pattern_count > 0, ESP_ERR_INVALID_ARG, "crt_tile",
                        "null pattern table");
    ESP_RETURN_ON_FALSE(pattern_count <= 256, ESP_ERR_INVALID_ARG, "crt_tile",
                        "pattern_count > 256");
    ESP_RETURN_ON_FALSE(nametable != NULL, ESP_ERR_INVALID_ARG, "crt_tile", "null nametable");

    *t = (crt_tile_layer_t){
        .visible_w_tiles = visible_w,
        .visible_h_tiles = visible_h,
        .pitch_w_tiles = pitch_w,
        .pitch_h_tiles = pitch_h,
        .pitch_w_mask = is_pow2(pitch_w) ? (uint16_t)(pitch_w - 1u) : 0u,
        .pitch_h_mask = is_pow2(pitch_h) ? (uint16_t)(pitch_h - 1u) : 0u,
        .pattern_table = pattern_table,
        .pattern_count = pattern_count,
        .nametable = nametable,
        .scroll_x_px = 0,
        .scroll_y_px = 0,
        .palette = NULL,
        .attributes = NULL,
        .palette_banks = NULL,
    };
    return ESP_OK;
}

/* ── Mutation ─────────────────────────────────────────────────────── */

void crt_tile_set_tile(crt_tile_layer_t *t, uint16_t col, uint16_t row, uint8_t tile_idx)
{
    if (t == NULL || t->nametable == NULL) {
        return;
    }
    if (col >= t->pitch_w_tiles || row >= t->pitch_h_tiles) {
        return;
    }
    t->nametable[(size_t)row * t->pitch_w_tiles + col] = tile_idx;
}

uint8_t crt_tile_get_tile(const crt_tile_layer_t *t, uint16_t col, uint16_t row)
{
    if (t == NULL || t->nametable == NULL) {
        return 0;
    }
    if (col >= t->pitch_w_tiles || row >= t->pitch_h_tiles) {
        return 0;
    }
    return t->nametable[(size_t)row * t->pitch_w_tiles + col];
}

void crt_tile_set_scroll(crt_tile_layer_t *t, int x_px, int y_px)
{
    if (t == NULL) {
        return;
    }
    const int vw_px = (int)t->visible_w_tiles * (int)CRT_TILE_PX_W;
    const int vh_px = (int)t->visible_h_tiles * (int)CRT_TILE_PX_H;
    t->scroll_x_px = wrap_u16(x_px, vw_px);
    t->scroll_y_px = wrap_u16(y_px, vh_px);
}

void crt_tile_set_palette(crt_tile_layer_t *t, const uint16_t *palette)
{
    if (t == NULL) {
        return;
    }
    t->palette = palette;
}

void crt_tile_set_attributes(crt_tile_layer_t *t, const uint8_t *attributes)
{
    if (t == NULL) {
        return;
    }
    t->attributes = attributes;
}

void crt_tile_set_palette_banks(crt_tile_layer_t *t, const crt_compose_palette_banks_t *banks)
{
    if (t == NULL) {
        return;
    }
    t->palette_banks = banks;
}

void crt_tile_set_attr(crt_tile_layer_t *t, uint16_t col, uint16_t row, uint8_t attr)
{
    if (t == NULL || t->attributes == NULL) {
        return;
    }
    if (col >= t->pitch_w_tiles || row >= t->pitch_h_tiles) {
        return;
    }
    ((uint8_t *)t->attributes)[(size_t)row * t->pitch_w_tiles + col] = attr;
}

uint8_t crt_tile_get_attr(const crt_tile_layer_t *t, uint16_t col, uint16_t row)
{
    if (t == NULL || t->attributes == NULL) {
        return 0;
    }
    if (col >= t->pitch_w_tiles || row >= t->pitch_h_tiles) {
        return 0;
    }
    return t->attributes[(size_t)row * t->pitch_w_tiles + col];
}

/* ── Hot path building blocks ─────────────────────────────────────── */

/* Compose one logical scanline (visible_w_tiles * 8 pixels) into @p out.
 * Caller owns the buffer. This works for both the fast path (direct
 * expansion consumer) and the generic fallback (fixed-point sampler). */
static IRAM_ATTR void tile_render_logical_line(const crt_tile_layer_t *t, uint16_t y,
                                               uint8_t *logical_out, uint8_t *attr_out)
{
    const uint16_t logical_w_px = (uint16_t)(t->visible_w_tiles * CRT_TILE_PX_W);
    const uint16_t logical_h_px = (uint16_t)(t->visible_h_tiles * CRT_TILE_PX_H);

    uint16_t screen_y = (uint16_t)(y + t->scroll_y_px);
    if (t->pitch_h_mask != 0u && logical_h_px == (uint16_t)(t->pitch_h_tiles * CRT_TILE_PX_H)) {
        /* pitch_h_tiles is PoT AND visible matches pitch on Y: AND wrap */
        screen_y = (uint16_t)(screen_y & (uint16_t)((t->pitch_h_tiles * CRT_TILE_PX_H) - 1u));
    } else {
        if (screen_y >= logical_h_px) {
            screen_y = (uint16_t)(screen_y % logical_h_px);
        }
    }
    const uint16_t tile_row_full = (uint16_t)(screen_y >> 3);
    const uint16_t fine_y = (uint16_t)(screen_y & 7u);
    const uint16_t tile_row = (t->pitch_h_mask != 0u)
                                  ? (uint16_t)(tile_row_full & t->pitch_h_mask)
                                  : (uint16_t)(tile_row_full % t->pitch_h_tiles);

    const uint8_t *nametable_row = &t->nametable[(size_t)tile_row * t->pitch_w_tiles];
    const uint8_t *pattern = t->pattern_table;
    const uint16_t pattern_count = t->pattern_count;

    /* Walk tile columns, emit 8 logical pixels per tile. */
    uint16_t scroll_tile_col = (uint16_t)(t->scroll_x_px >> 3);
    uint16_t scroll_fine_x = (uint16_t)(t->scroll_x_px & 7u);
    uint8_t *dst = logical_out;
    uint8_t *attr_dst = attr_out;
    uint16_t remaining = logical_w_px;

    uint16_t col = scroll_tile_col;
    uint16_t fine = scroll_fine_x;

    const uint8_t *attributes = t->attributes;
    if (attributes == NULL) {
        if (attr_out != NULL) {
            memset(attr_out, 0, logical_w_px);
        }
        while (remaining > 0u) {
            const uint16_t wrapped_col = (t->pitch_w_mask != 0u)
                                             ? (uint16_t)(col & t->pitch_w_mask)
                                             : (uint16_t)(col % t->pitch_w_tiles);
            uint8_t idx = nametable_row[wrapped_col];
            if (idx >= pattern_count) {
                idx = 0;
            }
            const uint8_t *tile_line =
                &pattern[(size_t)idx * CRT_TILE_BYTES + (size_t)fine_y * CRT_TILE_PX_W];
            uint16_t take = (uint16_t)(CRT_TILE_PX_W - fine);
            if (take > remaining) {
                take = remaining;
            }
            for (uint16_t i = 0; i < take; ++i) {
                dst[i] = tile_line[fine + i];
            }
            if (attr_dst != NULL) {
                memset(attr_dst, 0, take);
                attr_dst += take;
            }
            dst += take;
            remaining = (uint16_t)(remaining - take);
            fine = 0;
            col++;
        }
        return;
    }

    while (remaining > 0u) {
        const uint16_t wrapped_col = (t->pitch_w_mask != 0u) ? (uint16_t)(col & t->pitch_w_mask)
                                                             : (uint16_t)(col % t->pitch_w_tiles);
        const uint8_t attr = attributes[(size_t)tile_row * t->pitch_w_tiles + wrapped_col];
        uint8_t idx = nametable_row[wrapped_col];
        if (idx >= pattern_count) {
            idx = 0;
        }
        const uint16_t sample_y =
            (attr & CRT_TILE_ATTR_VFLIP) != 0u ? (uint16_t)(7u - fine_y) : fine_y;
        const uint8_t *tile_line =
            &pattern[(size_t)idx * CRT_TILE_BYTES + (size_t)sample_y * CRT_TILE_PX_W];
        uint16_t take = (uint16_t)(CRT_TILE_PX_W - fine);
        if (take > remaining) {
            take = remaining;
        }
        if ((attr & CRT_TILE_ATTR_HFLIP) != 0u) {
            for (uint16_t i = 0; i < take; ++i) {
                dst[i] = tile_line[7u - (fine + i)];
            }
        } else {
            for (uint16_t i = 0; i < take; ++i) {
                dst[i] = tile_line[fine + i];
            }
        }
        if (attr_dst != NULL) {
            const uint8_t compose_attr =
                (((attr & CRT_TILE_ATTR_PRIORITY) != 0u) ? CRT_COMPOSE_PIXEL_BG_PRIORITY : 0u) |
                (attr & CRT_TILE_ATTR_PALETTE_MASK);
            memset(attr_dst, compose_attr, take);
            attr_dst += take;
        }
        dst += take;
        remaining = (uint16_t)(remaining - take);
        fine = 0;
        col++;
    }
}

/* ── Compose layer adapter ────────────────────────────────────────── */

IRAM_ATTR bool crt_tile_layer_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                    uint16_t width)
{
    return crt_tile_layer_fetch_with_attrs(ctx, logical_line, idx_out, NULL, width);
}

IRAM_ATTR bool crt_tile_layer_fetch_with_attrs(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                               uint8_t *attr_out, uint16_t width)
{
    crt_tile_layer_t *t = (crt_tile_layer_t *)ctx;
    if (t == NULL || idx_out == NULL || width == 0u) {
        return false;
    }

    const uint16_t visible_h_px = (uint16_t)(t->visible_h_tiles * CRT_TILE_PX_H);
    if (logical_line >= visible_h_px) {
        memset(idx_out, 0, width);
        if (attr_out != NULL) {
            memset(attr_out, 0, width);
        }
        return true;
    }

    const uint16_t logical_w_px = (uint16_t)(t->visible_w_tiles * CRT_TILE_PX_W);

    /* Render one logical scanline into a stack buffer; max visible
     * width is 256 pixels for the common 32-tile case. Capped at
     * CRT_TILE_MAX_LOGICAL_W to bound stack usage on exotic configs. */
    uint8_t logical_line_buf[CRT_TILE_STACK_LOGICAL_W];
    uint8_t logical_attr_buf[CRT_TILE_STACK_LOGICAL_W];
    if (logical_w_px == 0u || logical_w_px > CRT_TILE_STACK_LOGICAL_W) {
        memset(idx_out, 0, width);
        if (attr_out != NULL) {
            memset(attr_out, 0, width);
        }
        return true;
    }
    tile_render_logical_line(t, logical_line, logical_line_buf,
                             (attr_out != NULL) ? logical_attr_buf : NULL);

    /* Fast path: exact 3:1 X expansion. 256 logical -> 768 DAC samples.
     * No fixed-point arithmetic in the inner loop. */
    if (logical_w_px == 256u && width == 768u) {
        uint8_t *dst = idx_out;
        for (uint16_t i = 0; i < 256u; ++i) {
            uint8_t v = logical_line_buf[i];
            dst[0] = v;
            dst[1] = v;
            dst[2] = v;
            dst += 3;
        }
        if (attr_out != NULL) {
            uint8_t *attr_dst = attr_out;
            for (uint16_t i = 0; i < 256u; ++i) {
                uint8_t v = logical_attr_buf[i];
                attr_dst[0] = v;
                attr_dst[1] = v;
                attr_dst[2] = v;
                attr_dst += 3;
            }
        }
        return true;
    }

    /* Generic fallback: fixed-point nearest-neighbor with CEILING
     * rounding in the step so integer-multiple expansions (e.g. 3:1)
     * collapse to the exact replication produced by the fast path. */
    uint32_t step = (((uint32_t)logical_w_px << 16) + (uint32_t)width - 1u) / (uint32_t)width;
    uint32_t acc = 0;
    for (uint16_t x = 0; x < width; ++x) {
        const uint16_t src_x = (uint16_t)(acc >> 16);
        idx_out[x] = logical_line_buf[src_x];
        if (attr_out != NULL) {
            attr_out[x] = logical_attr_buf[src_x];
        }
        acc += step;
    }
    return true;
}

/* ── Fused scanline hook ──────────────────────────────────────────── */

IRAM_ATTR static inline uint8_t tile_remap_palette_bank(const crt_compose_palette_banks_t *banks,
                                                        uint8_t attr, uint8_t idx)
{
    if (banks == NULL) {
        return idx;
    }
    const uint8_t bank_idx =
        (uint8_t)((attr & CRT_COMPOSE_PIXEL_BANK_MASK) >> CRT_COMPOSE_PIXEL_BANK_SHIFT);
    const uint8_t *bank = banks->banks[bank_idx];
    return (bank != NULL) ? bank[idx] : idx;
}

IRAM_ATTR static inline void tile_patch_expanded_palette_sample(uint16_t *active_buf,
                                                                uint16_t logical_x, uint16_t sample)
{
    const uint16_t first = (uint16_t)(logical_x * 3u);
    active_buf[first ^ 1u] = sample;
    active_buf[(first + 1u) ^ 1u] = sample;
    active_buf[(first + 2u) ^ 1u] = sample;
}

IRAM_ATTR static inline void
tile_write_expanded_palette_pair(uint16_t *active_buf, uint16_t logical_x, uint16_t l0, uint16_t l1)
{
    const uint16_t base = (uint16_t)(logical_x * 3u);
    active_buf[base] = l0;
    active_buf[base + 1u] = l0;
    active_buf[base + 2u] = l1;
    active_buf[base + 3u] = l0;
    active_buf[base + 4u] = l1;
    active_buf[base + 5u] = l1;
}

IRAM_ATTR static inline void tile_write_expanded_palette_full_tile_plain(uint16_t *active_buf,
                                                                         uint16_t x,
                                                                         const uint8_t *tile_line,
                                                                         const uint16_t *pal)
{
    if ((x & 1u) != 0u) {
        tile_patch_expanded_palette_sample(active_buf, x, pal[tile_line[0]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 1u), pal[tile_line[1]],
                                         pal[tile_line[2]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 3u), pal[tile_line[3]],
                                         pal[tile_line[4]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 5u), pal[tile_line[5]],
                                         pal[tile_line[6]]);
        tile_patch_expanded_palette_sample(active_buf, (uint16_t)(x + 7u), pal[tile_line[7]]);
        return;
    }
    tile_write_expanded_palette_pair(active_buf, x, pal[tile_line[0]], pal[tile_line[1]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 2u), pal[tile_line[2]],
                                     pal[tile_line[3]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 4u), pal[tile_line[4]],
                                     pal[tile_line[5]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 6u), pal[tile_line[6]],
                                     pal[tile_line[7]]);
}

IRAM_ATTR static inline void tile_write_expanded_palette_full_tile_bank(uint16_t *active_buf,
                                                                        uint16_t x,
                                                                        const uint8_t *tile_line,
                                                                        const uint8_t *bank,
                                                                        const uint16_t *pal)
{
    if ((x & 1u) != 0u) {
        tile_patch_expanded_palette_sample(active_buf, x, pal[bank[tile_line[0]]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 1u), pal[bank[tile_line[1]]],
                                         pal[bank[tile_line[2]]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 3u), pal[bank[tile_line[3]]],
                                         pal[bank[tile_line[4]]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 5u), pal[bank[tile_line[5]]],
                                         pal[bank[tile_line[6]]]);
        tile_patch_expanded_palette_sample(active_buf, (uint16_t)(x + 7u), pal[bank[tile_line[7]]]);
        return;
    }
    tile_write_expanded_palette_pair(active_buf, x, pal[bank[tile_line[0]]],
                                     pal[bank[tile_line[1]]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 2u), pal[bank[tile_line[2]]],
                                     pal[bank[tile_line[3]]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 4u), pal[bank[tile_line[4]]],
                                     pal[bank[tile_line[5]]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 6u), pal[bank[tile_line[6]]],
                                     pal[bank[tile_line[7]]]);
}

IRAM_ATTR static inline void tile_write_expanded_palette_full_tile_hflip(uint16_t *active_buf,
                                                                         uint16_t x,
                                                                         const uint8_t *tile_line,
                                                                         const uint16_t *pal)
{
    if ((x & 1u) != 0u) {
        tile_patch_expanded_palette_sample(active_buf, x, pal[tile_line[7]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 1u), pal[tile_line[6]],
                                         pal[tile_line[5]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 3u), pal[tile_line[4]],
                                         pal[tile_line[3]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 5u), pal[tile_line[2]],
                                         pal[tile_line[1]]);
        tile_patch_expanded_palette_sample(active_buf, (uint16_t)(x + 7u), pal[tile_line[0]]);
        return;
    }
    tile_write_expanded_palette_pair(active_buf, x, pal[tile_line[7]], pal[tile_line[6]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 2u), pal[tile_line[5]],
                                     pal[tile_line[4]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 4u), pal[tile_line[3]],
                                     pal[tile_line[2]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 6u), pal[tile_line[1]],
                                     pal[tile_line[0]]);
}

IRAM_ATTR static inline void
tile_write_expanded_palette_full_tile_bank_hflip(uint16_t *active_buf, uint16_t x,
                                                 const uint8_t *tile_line, const uint8_t *bank,
                                                 const uint16_t *pal)
{
    if ((x & 1u) != 0u) {
        tile_patch_expanded_palette_sample(active_buf, x, pal[bank[tile_line[7]]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 1u), pal[bank[tile_line[6]]],
                                         pal[bank[tile_line[5]]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 3u), pal[bank[tile_line[4]]],
                                         pal[bank[tile_line[3]]]);
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 5u), pal[bank[tile_line[2]]],
                                         pal[bank[tile_line[1]]]);
        tile_patch_expanded_palette_sample(active_buf, (uint16_t)(x + 7u), pal[bank[tile_line[0]]]);
        return;
    }
    tile_write_expanded_palette_pair(active_buf, x, pal[bank[tile_line[7]]],
                                     pal[bank[tile_line[6]]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 2u), pal[bank[tile_line[5]]],
                                     pal[bank[tile_line[4]]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 4u), pal[bank[tile_line[3]]],
                                     pal[bank[tile_line[2]]]);
    tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + 6u), pal[bank[tile_line[1]]],
                                     pal[bank[tile_line[0]]]);
}

IRAM_ATTR static inline void
tile_write_expanded_palette_plain_span(uint16_t *active_buf, uint16_t x, const uint8_t *tile_line,
                                       uint16_t fine, uint16_t take, const uint16_t *pal)
{
    if (fine == 0u && take == CRT_TILE_PX_W) {
        tile_write_expanded_palette_full_tile_plain(active_buf, x, tile_line, pal);
        return;
    }

    uint16_t i = 0;
    if ((x & 1u) != 0u && i < take) {
        tile_patch_expanded_palette_sample(active_buf, x, pal[tile_line[fine]]);
        i++;
    }
    for (; (uint16_t)(i + 1u) < take; i = (uint16_t)(i + 2u)) {
        const uint8_t sample0 = tile_line[fine + i];
        const uint8_t sample1 = tile_line[fine + i + 1u];
        tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + i), pal[sample0], pal[sample1]);
    }
    if (i < take) {
        tile_patch_expanded_palette_sample(active_buf, (uint16_t)(x + i), pal[tile_line[fine + i]]);
    }
}

IRAM_ATTR static void tile_write_expanded_palette_span(uint16_t *active_buf, uint16_t x,
                                                       const uint8_t *tile_line, uint16_t fine,
                                                       uint16_t take, const uint8_t *bank,
                                                       bool hflip, const uint16_t *pal)
{
    if (fine == 0u && take == CRT_TILE_PX_W) {
        if (!hflip && bank == NULL) {
            tile_write_expanded_palette_full_tile_plain(active_buf, x, tile_line, pal);
        } else if (!hflip) {
            tile_write_expanded_palette_full_tile_bank(active_buf, x, tile_line, bank, pal);
        } else if (bank == NULL) {
            tile_write_expanded_palette_full_tile_hflip(active_buf, x, tile_line, pal);
        } else {
            tile_write_expanded_palette_full_tile_bank_hflip(active_buf, x, tile_line, bank, pal);
        }
        return;
    }

    uint16_t i = 0;
    if ((x & 1u) != 0u && i < take) {
        const uint16_t sample_x = (uint16_t)(fine + i);
        const uint8_t raw = tile_line[hflip ? (uint16_t)(7u - sample_x) : sample_x];
        const uint8_t sample = (bank != NULL) ? bank[raw] : raw;
        tile_patch_expanded_palette_sample(active_buf, x, pal[sample]);
        i++;
    }
    if (!hflip && bank == NULL) {
        for (; (uint16_t)(i + 1u) < take; i = (uint16_t)(i + 2u)) {
            const uint8_t sample0 = tile_line[fine + i];
            const uint8_t sample1 = tile_line[fine + i + 1u];
            tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + i), pal[sample0],
                                             pal[sample1]);
        }
    } else if (!hflip) {
        for (; (uint16_t)(i + 1u) < take; i = (uint16_t)(i + 2u)) {
            const uint8_t sample0 = bank[tile_line[fine + i]];
            const uint8_t sample1 = bank[tile_line[fine + i + 1u]];
            tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + i), pal[sample0],
                                             pal[sample1]);
        }
    } else if (bank == NULL) {
        for (; (uint16_t)(i + 1u) < take; i = (uint16_t)(i + 2u)) {
            const uint8_t sample0 = tile_line[7u - (fine + i)];
            const uint8_t sample1 = tile_line[7u - (fine + i + 1u)];
            tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + i), pal[sample0],
                                             pal[sample1]);
        }
    } else {
        for (; (uint16_t)(i + 1u) < take; i = (uint16_t)(i + 2u)) {
            const uint8_t sample0 = bank[tile_line[7u - (fine + i)]];
            const uint8_t sample1 = bank[tile_line[7u - (fine + i + 1u)]];
            tile_write_expanded_palette_pair(active_buf, (uint16_t)(x + i), pal[sample0],
                                             pal[sample1]);
        }
    }
    if (i < take) {
        const uint16_t logical_x = (uint16_t)(x + i);
        const uint16_t sample_x = (uint16_t)(fine + i);
        const uint8_t raw = tile_line[hflip ? (uint16_t)(7u - sample_x) : sample_x];
        const uint8_t sample = (bank != NULL) ? bank[raw] : raw;
        tile_patch_expanded_palette_sample(active_buf, logical_x, pal[sample]);
    }
}

IRAM_ATTR static void tile_render_banked_256_to_768(const crt_tile_layer_t *t, uint16_t y,
                                                    uint16_t *active_buf)
{
    const uint16_t logical_h_px = (uint16_t)(t->visible_h_tiles * CRT_TILE_PX_H);
    uint16_t screen_y = (uint16_t)(y + t->scroll_y_px);
    if (t->pitch_h_mask != 0u && logical_h_px == (uint16_t)(t->pitch_h_tiles * CRT_TILE_PX_H)) {
        screen_y = (uint16_t)(screen_y & (uint16_t)((t->pitch_h_tiles * CRT_TILE_PX_H) - 1u));
    } else if (screen_y >= logical_h_px) {
        screen_y = (uint16_t)(screen_y % logical_h_px);
    }
    const uint16_t tile_row_full = (uint16_t)(screen_y >> 3);
    const uint16_t fine_y = (uint16_t)(screen_y & 7u);
    const uint16_t tile_row = (t->pitch_h_mask != 0u)
                                  ? (uint16_t)(tile_row_full & t->pitch_h_mask)
                                  : (uint16_t)(tile_row_full % t->pitch_h_tiles);
    const uint8_t *nametable_row = &t->nametable[(size_t)tile_row * t->pitch_w_tiles];
    const uint8_t *pattern = t->pattern_table;
    const uint16_t pattern_count = t->pattern_count;
    uint16_t col = (uint16_t)(t->scroll_x_px >> 3);
    uint16_t fine = (uint16_t)(t->scroll_x_px & 7u);
    uint16_t remaining = CRT_TILE_STACK_LOGICAL_W;
    uint16_t x = 0;
    const bool bank0_identity = t->palette_banks->banks[0] == NULL;

    while (remaining > 0u) {
        const uint16_t wrapped_col = (t->pitch_w_mask != 0u) ? (uint16_t)(col & t->pitch_w_mask)
                                                             : (uint16_t)(col % t->pitch_w_tiles);
        const uint8_t attr = (t->attributes != NULL)
                                 ? t->attributes[(size_t)tile_row * t->pitch_w_tiles + wrapped_col]
                                 : 0u;
        uint8_t idx = nametable_row[wrapped_col];
        if (idx >= pattern_count) {
            idx = 0;
        }
        uint16_t take = (uint16_t)(CRT_TILE_PX_W - fine);
        if (take > remaining) {
            take = remaining;
        }
        if (attr == 0u && bank0_identity) {
            const uint8_t *tile_line =
                &pattern[(size_t)idx * CRT_TILE_BYTES + (size_t)fine_y * CRT_TILE_PX_W];
            tile_write_expanded_palette_plain_span(active_buf, x, tile_line, fine, take,
                                                   t->palette);
        } else {
            const uint16_t sample_y =
                (attr & CRT_TILE_ATTR_VFLIP) != 0u ? (uint16_t)(7u - fine_y) : fine_y;
            const uint8_t *tile_line =
                &pattern[(size_t)idx * CRT_TILE_BYTES + (size_t)sample_y * CRT_TILE_PX_W];
            const uint8_t bank_idx =
                (uint8_t)((attr & CRT_COMPOSE_PIXEL_BANK_MASK) >> CRT_COMPOSE_PIXEL_BANK_SHIFT);
            const uint8_t *bank = t->palette_banks->banks[bank_idx];
            const bool hflip = (attr & CRT_TILE_ATTR_HFLIP) != 0u;
            tile_write_expanded_palette_span(active_buf, x, tile_line, fine, take, bank, hflip,
                                             t->palette);
        }
        x = (uint16_t)(x + take);
        remaining = (uint16_t)(remaining - take);
        fine = 0;
        col++;
    }
}

IRAM_ATTR void crt_tile_scanline_hook(const crt_scanline_t *scanline, uint16_t *active_buf,
                                      uint16_t active_width, void *user_data)
{
    const crt_tile_layer_t *t = (const crt_tile_layer_t *)user_data;
    if (scanline == NULL || t == NULL || active_buf == NULL || active_width == 0u ||
        t->palette == NULL || !CRT_SCANLINE_HAS_LOGICAL(scanline)) {
        return;
    }

    const uint16_t visible_h_px = (uint16_t)(t->visible_h_tiles * CRT_TILE_PX_H);
    if (scanline->logical_line >= visible_h_px) {
        return;
    }

    const uint16_t logical_w_px = (uint16_t)(t->visible_w_tiles * CRT_TILE_PX_W);

    if (logical_w_px == 0u || logical_w_px > CRT_TILE_STACK_LOGICAL_W) {
        return;
    }
    const bool use_palette_banks = t->palette_banks != NULL;

    /* Fast path: 256 logical -> 768 DAC with palette + I2S word-swap
     * fused into a single pass. Each logical pixel expands to 3 samples;
     * each pair of consecutive logical pixels (l0, l1) produces 6 output
     * samples split across 3 DAC pairs (pre-swap layout, then swapped):
     *
     *   pair (6p+0, 6p+1) = (l0, l0) -> swap = (l0, l0)
     *   pair (6p+2, 6p+3) = (l0, l1) -> swap = (l1, l0)
     *   pair (6p+4, 6p+5) = (l1, l1) -> swap = (l1, l1)
     *
     * 128 logical-pixel-pairs x 6 samples = 768 outputs. */
    if (logical_w_px == 256u && active_width == 768u) {
        if (use_palette_banks) {
            tile_render_banked_256_to_768(t, scanline->logical_line, active_buf);
            return;
        }
        uint8_t logical_line_buf[CRT_TILE_STACK_LOGICAL_W];
        tile_render_logical_line(t, scanline->logical_line, logical_line_buf, NULL);
        const uint16_t *pal = t->palette;
        for (uint16_t p = 0; p < 128u; ++p) {
            const uint16_t x0 = (uint16_t)(p * 2u);
            const uint16_t x1 = (uint16_t)(x0 + 1u);
            uint16_t l0 = pal[logical_line_buf[x0]];
            uint16_t l1 = pal[logical_line_buf[x1]];
            const uint16_t base = (uint16_t)(p * 6u);
            active_buf[base] = l0;
            active_buf[base + 1] = l0;
            active_buf[base + 2] = l1;
            active_buf[base + 3] = l0;
            active_buf[base + 4] = l1;
            active_buf[base + 5] = l1;
        }
        return;
    }

    /* Generic fallback: ceiling-step fixed-point + palette + swap.
     * Ceiling step aligns with the fast-path 3:1 replication so both
     * paths produce bit-identical output for matching dimensions. */
    uint8_t logical_line_buf[CRT_TILE_STACK_LOGICAL_W];
    uint8_t logical_attr_buf[CRT_TILE_STACK_LOGICAL_W];
    tile_render_logical_line(t, scanline->logical_line, logical_line_buf,
                             use_palette_banks ? logical_attr_buf : NULL);
    const uint16_t *pal = t->palette;
    const crt_compose_palette_banks_t *banks = t->palette_banks;
    uint32_t step =
        (((uint32_t)logical_w_px << 16) + (uint32_t)active_width - 1u) / (uint32_t)active_width;
    uint32_t acc = 0;
    const uint16_t even_width = active_width & (uint16_t)~1U;
    uint16_t i = 0;
    for (; i < even_width; i += 2) {
        const uint16_t x0 = (uint16_t)(acc >> 16);
        const uint8_t i0 = tile_remap_palette_bank(
            banks, use_palette_banks ? logical_attr_buf[x0] : 0u, logical_line_buf[x0]);
        uint16_t p0 = pal[i0];
        acc += step;
        const uint16_t x1 = (uint16_t)(acc >> 16);
        const uint8_t i1 = tile_remap_palette_bank(
            banks, use_palette_banks ? logical_attr_buf[x1] : 0u, logical_line_buf[x1]);
        uint16_t p1 = pal[i1];
        acc += step;
        active_buf[i] = p1;
        active_buf[i + 1] = p0;
    }
    if (i < active_width) {
        const uint16_t x = (uint16_t)(acc >> 16);
        const uint8_t idx = tile_remap_palette_bank(
            banks, use_palette_banks ? logical_attr_buf[x] : 0u, logical_line_buf[x]);
        active_buf[i] = pal[idx];
    }
}
