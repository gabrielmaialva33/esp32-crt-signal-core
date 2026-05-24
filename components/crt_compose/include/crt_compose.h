#ifndef CRT_COMPOSE_H
#define CRT_COMPOSE_H

#include "crt_scanline.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file crt_compose.h
 * @brief Per-scanline indexed-8 compositor with layer z-order, keyed
 *        transparency, palette LUT and I2S word-swap output.
 *
 * The compositor sits on top of the scanline hook ABI. Each layer is a
 * fetch callback that emits one indexed-8 line into a scratch buffer.
 * Layers are resolved back-to-front; later layers overwrite earlier ones
 * unless the pixel equals the layer's transparency key. Attribute-aware
 * layers may also publish per-column priority flags and palette-bank indices.
 * Sprite pixels win over tile BG only when both CRT_COMPOSE_PIXEL_BG_PRIORITY
 * and CRT_COMPOSE_PIXEL_SPRITE_BG_PRIO are clear; unlike some PPUs, BG index 0
 * is still opaque for this priority rule. Palette banks optionally remap final
 * indexed-8 pixels before the single final pass maps indices through the
 * palette LUT and writes I2S-swapped 16-bit DAC samples into the active region
 * buffer.
 *
 * Usage:
 *     static crt_compose_t compositor;
 *     crt_compose_init(&compositor);
 *     crt_compose_set_palette(&compositor, my_palette, 256);
 *     crt_compose_add_layer(&compositor, bg_tile_fetch, &bg_ctx,
 *                           CRT_COMPOSE_NO_TRANSPARENCY);
 *     crt_compose_add_layer(&compositor, sprite_fetch, &spr_ctx, 0);
 *     crt_register_scanline_hook(crt_compose_scanline_hook, &compositor);
 */

#ifndef CRT_COMPOSE_MAX_WIDTH
#define CRT_COMPOSE_MAX_WIDTH 1024
#endif

#ifndef CRT_COMPOSE_MAX_LAYERS
#define CRT_COMPOSE_MAX_LAYERS 8
#endif

/** Sentinel for layers without keyed transparency (fully opaque overwrite). */
#define CRT_COMPOSE_NO_TRANSPARENCY ((uint16_t)0xFFFFu)

/** Sentinel returned by layer-creation helpers when no layer was created. */
#define CRT_COMPOSE_LAYER_INVALID ((uint8_t)0xFFu)

/** Current composed BG pixel should stay over sprite pixels. */
#define CRT_COMPOSE_PIXEL_BG_PRIORITY (1u << 0)

/** Candidate sprite pixel requests "behind BG" priority. */
#define CRT_COMPOSE_PIXEL_SPRITE_BG_PRIO (1u << 1)

/** Candidate pixel came from a sprite layer; used to scope BG priority merges. */
#define CRT_COMPOSE_PIXEL_SPRITE_OPAQUE (1u << 2)

/** Palette-bank selector bits carried from attribute-aware layers. */
#define CRT_COMPOSE_PIXEL_BANK_SHIFT 3
#define CRT_COMPOSE_PIXEL_BANK_MASK  (0x0Fu << CRT_COMPOSE_PIXEL_BANK_SHIFT)

#define CRT_COMPOSE_MAX_PALETTE_BANKS 16

typedef struct {
    /** Each bank maps indexed-8 pattern bytes to final DAC palette indices. */
    const uint8_t *banks[CRT_COMPOSE_MAX_PALETTE_BANKS];
} crt_compose_palette_banks_t;

/**
 * @brief Layer fetch callback.
 *
 * Produces one indexed-8 scanline into @p idx_out. The callback must
 * not allocate, block, log, or touch peripherals. The fetch runs from
 * the prep task, not from ISR context, so deterministic C arithmetic
 * is fine.
 *
 * Return `true` when the layer is present on this line and the caller
 * should consume/merge @p idx_out. Return `false` to indicate the layer
 * is fully transparent on this line; the caller will skip the merge pass
 * entirely, saving a memset and a full-line merge loop. Opaque base
 * layers (`CRT_COMPOSE_NO_TRANSPARENCY`) must always return `true`.
 *
 * @param ctx          Layer-private context provided at registration.
 * @param logical_line Visible line index in `0..active_lines - 1`.
 * @param idx_out      Destination scratch buffer, length @p width.
 * @param width        Number of pixels to emit.
 * @return             True if the layer contributed to this line.
 */
typedef bool (*crt_layer_fetch_fn)(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                   uint16_t width);

/**
 * @brief Attribute-aware layer fetch callback.
 *
 * Produces one indexed-8 scanline into @p idx_out and optional per-pixel
 * compose flags into @p attr_out. @p attr_out may be NULL; callbacks must then
 * behave like crt_layer_fetch_fn.
 */
typedef bool (*crt_layer_fetch_attr_fn)(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                        uint8_t *attr_out, uint16_t width);

typedef struct {
    crt_layer_fetch_fn fetch;
    crt_layer_fetch_attr_fn fetch_attr;
    /**
     * Optional fused scanline hook. When set, and when this layer is the
     * only enabled opaque layer in the stack, compose delegates the whole
     * scanline directly to this callback and skips its own indexed line
     * buffer + palette pass entirely. Use this to give back the fused
     * fetch+palette+swap write that a bare scanline hook would perform.
     * May be NULL.
     */
    crt_scanline_hook_fn scanline_override;
    void *ctx;
    /**
     * Index value treated as transparent. Pixels matching this value do
     * not overwrite the composited line. Use `CRT_COMPOSE_NO_TRANSPARENCY`
     * for fully opaque layers (e.g. the bottom background).
     */
    uint16_t transparent_idx;
    bool enabled;
} crt_compose_layer_t;

typedef struct {
    crt_layer_fetch_fn fetch;
    crt_layer_fetch_attr_fn fetch_attr;
    crt_scanline_hook_fn scanline_override;
    void *ctx;
    uint16_t transparent_idx;
    bool enabled;
} crt_compose_layer_info_t;

typedef struct {
    crt_compose_layer_t layers[CRT_COMPOSE_MAX_LAYERS];
    uint8_t layer_count;

    /** Base index written to the line buffer before any layer runs. */
    uint8_t clear_idx;

    /** 256-entry DAC LUT indexed by pixel value; NULL disables output. */
    const uint16_t *palette;
    uint16_t palette_size;

    /** Optional indexed-8 palette-bank remap tables; NULL means identity. */
    const crt_compose_palette_banks_t *palette_banks;

    /** Scratch buffers live inside the struct to avoid stack pressure. */
    uint8_t line[CRT_COMPOSE_MAX_WIDTH];
    uint8_t scratch[CRT_COMPOSE_MAX_WIDTH];
    uint8_t attr_line[CRT_COMPOSE_MAX_WIDTH];
    uint8_t attr_scratch[CRT_COMPOSE_MAX_WIDTH];
} crt_compose_t;

/* ── Lifecycle ────────────────────────────────────────────────────── */

esp_err_t crt_compose_init(crt_compose_t *c);

esp_err_t crt_compose_set_palette(crt_compose_t *c, const uint16_t *palette, uint16_t size);

void crt_compose_set_palette_banks(crt_compose_t *c, const crt_compose_palette_banks_t *banks);

const crt_compose_palette_banks_t *crt_compose_get_palette_banks(const crt_compose_t *c);

void crt_compose_set_clear_index(crt_compose_t *c, uint8_t idx);

/* ── Layer management ─────────────────────────────────────────────── */

/**
 * @brief Append a layer on top of the existing stack.
 *
 * Layer 0 is the back; the last added layer is the front.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the layer stack is full.
 */
esp_err_t crt_compose_add_layer(crt_compose_t *c, crt_layer_fetch_fn fetch, void *ctx,
                                uint16_t transparent_idx);

esp_err_t crt_compose_add_layer_with_attrs(crt_compose_t *c, crt_layer_fetch_attr_fn fetch,
                                           void *ctx, uint16_t transparent_idx);

/**
 * @brief Append a layer and return its stack index.
 *
 * Use this when the caller needs to mutate the layer later, for example to
 * toggle visibility, change its transparency key, or reorder priority.
 */
esp_err_t crt_compose_add_layer_with_id(crt_compose_t *c, crt_layer_fetch_fn fetch, void *ctx,
                                        uint16_t transparent_idx, uint8_t *out_layer_idx);

esp_err_t crt_compose_add_layer_with_attrs_with_id(crt_compose_t *c, crt_layer_fetch_attr_fn fetch,
                                                   void *ctx, uint16_t transparent_idx,
                                                   uint8_t *out_layer_idx);

/**
 * @brief Append an opaque layer that also exposes a fused scanline hook.
 *
 * The fused @p scanline_override is called directly when this layer is the
 * only active opaque layer, giving bit-for-bit parity with the original
 * hook path. When other layers become active, compose falls back to the
 * generic fetch+merge path using @p fetch.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the layer stack is full.
 */
esp_err_t crt_compose_add_layer_fused(crt_compose_t *c, crt_layer_fetch_fn fetch,
                                      crt_scanline_hook_fn scanline_override, void *ctx);

esp_err_t crt_compose_add_layer_fused_with_attrs(crt_compose_t *c, crt_layer_fetch_attr_fn fetch,
                                                 crt_scanline_hook_fn scanline_override, void *ctx);

/**
 * @brief Append a fused opaque layer and return its stack index.
 */
esp_err_t crt_compose_add_layer_fused_with_id(crt_compose_t *c, crt_layer_fetch_fn fetch,
                                              crt_scanline_hook_fn scanline_override, void *ctx,
                                              uint8_t *out_layer_idx);

esp_err_t crt_compose_add_layer_fused_with_attrs_with_id(crt_compose_t *c,
                                                         crt_layer_fetch_attr_fn fetch,
                                                         crt_scanline_hook_fn scanline_override,
                                                         void *ctx, uint8_t *out_layer_idx);

void crt_compose_clear_layers(crt_compose_t *c);

void crt_compose_set_layer_enabled(crt_compose_t *c, uint8_t layer_idx, bool enabled);

esp_err_t crt_compose_get_layer_info(const crt_compose_t *c, uint8_t layer_idx,
                                     crt_compose_layer_info_t *out_info);

esp_err_t crt_compose_set_layer_fetch(crt_compose_t *c, uint8_t layer_idx, crt_layer_fetch_fn fetch,
                                      void *ctx);

esp_err_t crt_compose_set_layer_context(crt_compose_t *c, uint8_t layer_idx, void *ctx);

esp_err_t crt_compose_set_layer_transparent_index(crt_compose_t *c, uint8_t layer_idx,
                                                  uint16_t transparent_idx);

esp_err_t crt_compose_swap_layers(crt_compose_t *c, uint8_t first_layer_idx,
                                  uint8_t second_layer_idx);

/* ── Scanline hook ────────────────────────────────────────────────── */

/**
 * @brief Scanline hook that composites all enabled layers and emits
 *        palette-mapped, I2S-swapped DAC samples into @p active_buf.
 *
 * Register with: `crt_register_scanline_hook(crt_compose_scanline_hook, &c);`
 */
void crt_compose_scanline_hook(const crt_scanline_t *scanline, uint16_t *active_buf,
                               uint16_t active_width, void *user_data);

/**
 * @brief RGB332 composite-color scanline hook for a 256-pixel logical scene.
 *
 * Composes all enabled layers into a 256-byte indexed/RGB332 logical line,
 * then expands it to 768 DAC samples using the composite RGB332 encoder.
 * This keeps the existing palette/DAC hook intact while adding the PPU-style
 * tile + sprite path.
 */
void crt_compose_scanline_hook_rgb332_256(const crt_scanline_t *scanline, uint16_t *active_buf,
                                          uint16_t active_width, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* CRT_COMPOSE_H */
