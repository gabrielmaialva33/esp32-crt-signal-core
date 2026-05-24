#include "crt_compose.h"

#include "crt_composite_palette.h"
#include "crt_sprite.h"

#include "esp_attr.h"
#include "esp_check.h"

#include <string.h>

/* ── Lifecycle ────────────────────────────────────────────────────── */

esp_err_t crt_compose_init(crt_compose_t *c)
{
    ESP_RETURN_ON_FALSE(c != NULL, ESP_ERR_INVALID_ARG, "crt_compose", "null state");
    memset(c, 0, sizeof(*c));
    c->palette_banks = NULL;
    return ESP_OK;
}

esp_err_t crt_compose_set_palette(crt_compose_t *c, const uint16_t *palette, uint16_t size)
{
    ESP_RETURN_ON_FALSE(c != NULL, ESP_ERR_INVALID_ARG, "crt_compose", "null state");
    ESP_RETURN_ON_FALSE(palette == NULL || size >= 256U, ESP_ERR_INVALID_SIZE, "crt_compose",
                        "indexed-8 palette requires 256 entries");
    c->palette = palette;
    c->palette_size = (palette != NULL) ? size : 0;
    return ESP_OK;
}

void crt_compose_set_palette_banks(crt_compose_t *c, const crt_compose_palette_banks_t *banks)
{
    if (c != NULL) {
        c->palette_banks = banks;
    }
}

const crt_compose_palette_banks_t *crt_compose_get_palette_banks(const crt_compose_t *c)
{
    return (c != NULL) ? c->palette_banks : NULL;
}

void crt_compose_set_clear_index(crt_compose_t *c, uint8_t idx)
{
    if (c != NULL) {
        c->clear_idx = idx;
    }
}

void crt_compose_reset_stats(crt_compose_t *c)
{
    if (c != NULL) {
        memset(&c->stats, 0, sizeof(c->stats));
    }
}

crt_compose_stats_t crt_compose_get_stats(const crt_compose_t *c)
{
    return (c != NULL) ? c->stats : (crt_compose_stats_t){0};
}

/* ── Layer management ─────────────────────────────────────────────── */

static esp_err_t crt_compose_append_layer(crt_compose_t *c, crt_layer_fetch_fn fetch,
                                          crt_layer_fetch_attr_fn fetch_attr,
                                          crt_scanline_hook_fn scanline_override, void *ctx,
                                          uint16_t transparent_idx, uint8_t *out_layer_idx)
{
    ESP_RETURN_ON_FALSE(c != NULL, ESP_ERR_INVALID_ARG, "crt_compose", "null state");
    ESP_RETURN_ON_FALSE(fetch != NULL || fetch_attr != NULL, ESP_ERR_INVALID_ARG, "crt_compose",
                        "null fetch");
    ESP_RETURN_ON_FALSE(c->layer_count < CRT_COMPOSE_MAX_LAYERS, ESP_ERR_NO_MEM, "crt_compose",
                        "layer stack full");

    const uint8_t layer_idx = c->layer_count;
    c->layers[c->layer_count] = (crt_compose_layer_t){
        .fetch = fetch,
        .fetch_attr = fetch_attr,
        .scanline_override = scanline_override,
        .ctx = ctx,
        .transparent_idx = transparent_idx,
        .enabled = true,
    };
    c->layer_count++;
    if (out_layer_idx != NULL) {
        *out_layer_idx = layer_idx;
    }
    return ESP_OK;
}

esp_err_t crt_compose_add_layer(crt_compose_t *c, crt_layer_fetch_fn fetch, void *ctx,
                                uint16_t transparent_idx)
{
    return crt_compose_append_layer(c, fetch, NULL, NULL, ctx, transparent_idx, NULL);
}

esp_err_t crt_compose_add_layer_with_attrs(crt_compose_t *c, crt_layer_fetch_attr_fn fetch,
                                           void *ctx, uint16_t transparent_idx)
{
    return crt_compose_append_layer(c, NULL, fetch, NULL, ctx, transparent_idx, NULL);
}

esp_err_t crt_compose_add_layer_with_id(crt_compose_t *c, crt_layer_fetch_fn fetch, void *ctx,
                                        uint16_t transparent_idx, uint8_t *out_layer_idx)
{
    if (out_layer_idx != NULL) {
        *out_layer_idx = CRT_COMPOSE_LAYER_INVALID;
    }
    return crt_compose_append_layer(c, fetch, NULL, NULL, ctx, transparent_idx, out_layer_idx);
}

esp_err_t crt_compose_add_layer_with_attrs_with_id(crt_compose_t *c, crt_layer_fetch_attr_fn fetch,
                                                   void *ctx, uint16_t transparent_idx,
                                                   uint8_t *out_layer_idx)
{
    if (out_layer_idx != NULL) {
        *out_layer_idx = CRT_COMPOSE_LAYER_INVALID;
    }
    return crt_compose_append_layer(c, NULL, fetch, NULL, ctx, transparent_idx, out_layer_idx);
}

esp_err_t crt_compose_add_layer_fused(crt_compose_t *c, crt_layer_fetch_fn fetch,
                                      crt_scanline_hook_fn scanline_override, void *ctx)
{
    return crt_compose_append_layer(c, fetch, NULL, scanline_override, ctx,
                                    CRT_COMPOSE_NO_TRANSPARENCY, NULL);
}

esp_err_t crt_compose_add_layer_fused_with_attrs(crt_compose_t *c, crt_layer_fetch_attr_fn fetch,
                                                 crt_scanline_hook_fn scanline_override, void *ctx)
{
    return crt_compose_append_layer(c, NULL, fetch, scanline_override, ctx,
                                    CRT_COMPOSE_NO_TRANSPARENCY, NULL);
}

esp_err_t crt_compose_add_layer_fused_with_id(crt_compose_t *c, crt_layer_fetch_fn fetch,
                                              crt_scanline_hook_fn scanline_override, void *ctx,
                                              uint8_t *out_layer_idx)
{
    if (out_layer_idx != NULL) {
        *out_layer_idx = CRT_COMPOSE_LAYER_INVALID;
    }
    return crt_compose_append_layer(c, fetch, NULL, scanline_override, ctx,
                                    CRT_COMPOSE_NO_TRANSPARENCY, out_layer_idx);
}

esp_err_t crt_compose_add_layer_fused_with_attrs_with_id(crt_compose_t *c,
                                                         crt_layer_fetch_attr_fn fetch,
                                                         crt_scanline_hook_fn scanline_override,
                                                         void *ctx, uint8_t *out_layer_idx)
{
    if (out_layer_idx != NULL) {
        *out_layer_idx = CRT_COMPOSE_LAYER_INVALID;
    }
    return crt_compose_append_layer(c, NULL, fetch, scanline_override, ctx,
                                    CRT_COMPOSE_NO_TRANSPARENCY, out_layer_idx);
}

void crt_compose_clear_layers(crt_compose_t *c)
{
    if (c != NULL) {
        c->layer_count = 0;
    }
}

void crt_compose_set_layer_enabled(crt_compose_t *c, uint8_t layer_idx, bool enabled)
{
    if (c != NULL && layer_idx < c->layer_count) {
        c->layers[layer_idx].enabled = enabled;
    }
}

static bool crt_compose_layer_idx_valid(const crt_compose_t *c, uint8_t layer_idx)
{
    return c != NULL && layer_idx < c->layer_count;
}

esp_err_t crt_compose_get_layer_info(const crt_compose_t *c, uint8_t layer_idx,
                                     crt_compose_layer_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(crt_compose_layer_idx_valid(c, layer_idx), ESP_ERR_INVALID_ARG,
                        "crt_compose", "invalid layer");
    ESP_RETURN_ON_FALSE(out_info != NULL, ESP_ERR_INVALID_ARG, "crt_compose", "null out_info");

    const crt_compose_layer_t *layer = &c->layers[layer_idx];
    *out_info = (crt_compose_layer_info_t){
        .fetch = layer->fetch,
        .fetch_attr = layer->fetch_attr,
        .scanline_override = layer->scanline_override,
        .ctx = layer->ctx,
        .transparent_idx = layer->transparent_idx,
        .enabled = layer->enabled,
    };
    return ESP_OK;
}

esp_err_t crt_compose_set_layer_fetch(crt_compose_t *c, uint8_t layer_idx, crt_layer_fetch_fn fetch,
                                      void *ctx)
{
    ESP_RETURN_ON_FALSE(crt_compose_layer_idx_valid(c, layer_idx), ESP_ERR_INVALID_ARG,
                        "crt_compose", "invalid layer");
    ESP_RETURN_ON_FALSE(fetch != NULL, ESP_ERR_INVALID_ARG, "crt_compose", "null fetch");

    c->layers[layer_idx].fetch = fetch;
    c->layers[layer_idx].fetch_attr = NULL;
    c->layers[layer_idx].ctx = ctx;
    return ESP_OK;
}

esp_err_t crt_compose_set_layer_context(crt_compose_t *c, uint8_t layer_idx, void *ctx)
{
    ESP_RETURN_ON_FALSE(crt_compose_layer_idx_valid(c, layer_idx), ESP_ERR_INVALID_ARG,
                        "crt_compose", "invalid layer");

    c->layers[layer_idx].ctx = ctx;
    return ESP_OK;
}

esp_err_t crt_compose_set_layer_transparent_index(crt_compose_t *c, uint8_t layer_idx,
                                                  uint16_t transparent_idx)
{
    ESP_RETURN_ON_FALSE(crt_compose_layer_idx_valid(c, layer_idx), ESP_ERR_INVALID_ARG,
                        "crt_compose", "invalid layer");

    c->layers[layer_idx].transparent_idx = transparent_idx;
    return ESP_OK;
}

esp_err_t crt_compose_swap_layers(crt_compose_t *c, uint8_t first_layer_idx,
                                  uint8_t second_layer_idx)
{
    ESP_RETURN_ON_FALSE(crt_compose_layer_idx_valid(c, first_layer_idx), ESP_ERR_INVALID_ARG,
                        "crt_compose", "invalid first layer");
    ESP_RETURN_ON_FALSE(crt_compose_layer_idx_valid(c, second_layer_idx), ESP_ERR_INVALID_ARG,
                        "crt_compose", "invalid second layer");

    if (first_layer_idx == second_layer_idx) {
        return ESP_OK;
    }

    crt_compose_layer_t tmp = c->layers[first_layer_idx];
    c->layers[first_layer_idx] = c->layers[second_layer_idx];
    c->layers[second_layer_idx] = tmp;
    return ESP_OK;
}

/* ── Scanline hook ────────────────────────────────────────────────── */

static inline bool crt_compose_layer_has_fetch(const crt_compose_layer_t *layer)
{
    return layer->fetch != NULL || layer->fetch_attr != NULL;
}

IRAM_ATTR static void crt_compose_stats_record(crt_compose_t *c, bool materialized,
                                               uint8_t layers_fetched)
{
    if (materialized) {
        c->stats.materialized_lines++;
    } else {
        c->stats.fused_lines++;
    }
    if (layers_fetched > c->stats.max_layers_fetched) {
        c->stats.max_layers_fetched = layers_fetched;
    }
}

IRAM_ATTR static bool crt_compose_fetch_layer(const crt_compose_layer_t *layer,
                                              uint16_t logical_line, uint8_t *idx_out,
                                              uint8_t *attr_out, uint16_t width)
{
    if (layer->fetch_attr != NULL) {
        return layer->fetch_attr(layer->ctx, logical_line, idx_out, attr_out, width);
    }
    return layer->fetch(layer->ctx, logical_line, idx_out, width);
}

IRAM_ATTR static bool crt_compose_sprite_pixel_blocked(uint8_t bg_attr, uint8_t sprite_attr)
{
    return ((bg_attr & CRT_COMPOSE_PIXEL_BG_PRIORITY) != 0u) ||
           ((sprite_attr & CRT_COMPOSE_PIXEL_SPRITE_BG_PRIO) != 0u);
}

IRAM_ATTR static void crt_compose_merge_keyed_line(crt_compose_t *c, uint16_t width, uint8_t key)
{
    for (uint16_t x = 0; x < width; ++x) {
        uint8_t s = c->scratch[x];
        if (s == key) {
            continue;
        }
        const uint8_t attr = c->attr_scratch[x];
        if ((attr & CRT_COMPOSE_PIXEL_SPRITE_OPAQUE) != 0u &&
            crt_compose_sprite_pixel_blocked(c->attr_line[x], attr)) {
            continue;
        }
        c->line[x] = s;
        c->attr_line[x] = attr;
    }
}

IRAM_ATTR static void crt_compose_apply_palette_banks(crt_compose_t *c, uint16_t width)
{
    const crt_compose_palette_banks_t *banks = c->palette_banks;
    if (banks == NULL) {
        return;
    }

    for (uint16_t x = 0; x < width; ++x) {
        const uint8_t bank_idx = (uint8_t)((c->attr_line[x] & CRT_COMPOSE_PIXEL_BANK_MASK) >>
                                           CRT_COMPOSE_PIXEL_BANK_SHIFT);
        const uint8_t *bank = banks->banks[bank_idx];
        if (bank != NULL) {
            c->line[x] = bank[c->line[x]];
        }
    }
}

IRAM_ATTR static void crt_compose_patch_expanded_palette_sample(uint16_t *active_buf,
                                                                uint16_t logical_x, uint16_t sample)
{
    const uint16_t first = (uint16_t)(logical_x * 3u);
    for (uint8_t repeat = 0; repeat < 3u; ++repeat) {
        const uint16_t stream_pos = (uint16_t)(first + repeat);
        active_buf[stream_pos ^ 1u] = sample;
    }
}

IRAM_ATTR static bool crt_compose_patch_sprite_spans_256(crt_compose_t *c,
                                                         const crt_compose_layer_t *sprite_layer,
                                                         uint16_t logical_line,
                                                         uint16_t *active_buf)
{
    crt_sprite_layer_t *sprites = (crt_sprite_layer_t *)sprite_layer->ctx;
    crt_sprite_scanline_span_t spans[CRT_SPRITE_DEFAULT_PERLINE];
    if (sprites == NULL || sprites->max_sprites_per_line > CRT_SPRITE_DEFAULT_PERLINE) {
        return false;
    }
    const uint8_t span_count = crt_sprite_layer_collect_scanline(
        sprites, logical_line, CRT_COMPOSITE_RGB332_WIDTH, spans, CRT_SPRITE_DEFAULT_PERLINE);
    if (span_count == 0) {
        return true;
    }

    const uint8_t key = (uint8_t)sprite_layer->transparent_idx;
    const uint16_t *pal = c->palette;
    const crt_compose_palette_banks_t *banks = c->palette_banks;
    for (uint8_t si = 0; si < span_count; ++si) {
        const crt_sprite_scanline_span_t *span = &spans[si];
        if ((span->attr & CRT_COMPOSE_PIXEL_SPRITE_BG_PRIO) != 0u) {
            continue;
        }
        const uint8_t bank_idx =
            (uint8_t)((span->attr & CRT_COMPOSE_PIXEL_BANK_MASK) >> CRT_COMPOSE_PIXEL_BANK_SHIFT);
        const uint8_t *bank = (banks != NULL) ? banks->banks[bank_idx] : NULL;
        const uint8_t *src = span->src;
        for (uint8_t sx = 0; sx < span->width; ++sx) {
            const uint8_t sample_idx = *src;
            if (sample_idx != key) {
                const uint8_t palette_idx = (bank != NULL) ? bank[sample_idx] : sample_idx;
                crt_compose_patch_expanded_palette_sample(active_buf, (uint16_t)(span->dst_x + sx),
                                                          pal[palette_idx]);
            }
            src += span->src_step;
        }
    }
    return true;
}

IRAM_ATTR static uint8_t crt_compose_render_indexed_line(crt_compose_t *c, uint16_t logical_line,
                                                         uint16_t width)
{
    bool line_ready = false;
    uint8_t layers_fetched = 0;

    for (uint8_t li = 0; li < c->layer_count; ++li) {
        const crt_compose_layer_t *layer = &c->layers[li];
        if (!layer->enabled || !crt_compose_layer_has_fetch(layer)) {
            continue;
        }

        if (layer->transparent_idx == CRT_COMPOSE_NO_TRANSPARENCY) {
            memset(c->attr_line, 0, width);
            layers_fetched++;
            (void)crt_compose_fetch_layer(layer, logical_line, c->line, c->attr_line, width);
            line_ready = true;
            continue;
        }

        if (!line_ready) {
            memset(c->line, c->clear_idx, width);
            memset(c->attr_line, 0, width);
            line_ready = true;
        }

        memset(c->attr_scratch, 0, width);
        layers_fetched++;
        if (!crt_compose_fetch_layer(layer, logical_line, c->scratch, c->attr_scratch, width)) {
            continue;
        }

        const uint8_t key = (uint8_t)layer->transparent_idx;
        crt_compose_merge_keyed_line(c, width, key);
    }

    if (!line_ready) {
        memset(c->line, c->clear_idx, width);
        memset(c->attr_line, 0, width);
    }

    crt_compose_apply_palette_banks(c, width);
    return layers_fetched;
}

IRAM_ATTR static void crt_compose_render_palette_line(const crt_compose_t *c, uint16_t *active_buf,
                                                      uint16_t active_width, uint16_t logical_width)
{
    if (active_width == CRT_COMPOSITE_RGB332_ACTIVE_WIDTH &&
        logical_width == CRT_COMPOSITE_RGB332_WIDTH) {
        crt_composite_palette_render_256_to_768(c->palette, c->line, active_buf);
        return;
    }

    const uint8_t *line = c->line;
    const uint16_t *pal = c->palette;
    const uint16_t even_width = logical_width & (uint16_t)~1U;
    uint16_t i = 0;

    for (; i < even_width; i += 2) {
        uint16_t p0 = pal[line[i]];
        uint16_t p1 = pal[line[i + 1]];
        active_buf[i] = p1;
        active_buf[i + 1] = p0;
    }
    if (i < logical_width) {
        active_buf[i] = pal[line[i]];
    }
}

IRAM_ATTR void crt_compose_scanline_hook(const crt_scanline_t *scanline, uint16_t *active_buf,
                                         uint16_t active_width, void *user_data)
{
    crt_compose_t *c = (crt_compose_t *)user_data;

    if (scanline == NULL || c == NULL || active_buf == NULL || active_width == 0 ||
        active_width > CRT_COMPOSE_MAX_WIDTH || c->palette == NULL ||
        !CRT_SCANLINE_HAS_LOGICAL(scanline)) {
        return;
    }

    const bool expand_256_to_768 = (active_width == CRT_COMPOSITE_RGB332_ACTIVE_WIDTH);
    const uint16_t logical_width = expand_256_to_768 ? CRT_COMPOSITE_RGB332_WIDTH : active_width;
    uint8_t layers_fetched = 0;

    /* Pre-scan: classify the active layer stack for the hot path picker.
     * Two layer counts matter:
     *   opaque_count — layers with transparent_idx == NO_TRANSPARENCY
     *   keyed_count  — layers with keyed transparency
     * The fused base is the unique opaque layer when one exists and carries
     * a scanline_override. */
    int base_idx = -1;
    int keyed_idx = -1;
    uint8_t opaque_count = 0;
    uint8_t keyed_count = 0;
    for (uint8_t li = 0; li < c->layer_count; ++li) {
        const crt_compose_layer_t *layer = &c->layers[li];
        if (!layer->enabled || !crt_compose_layer_has_fetch(layer)) {
            continue;
        }
        if (layer->transparent_idx == CRT_COMPOSE_NO_TRANSPARENCY) {
            base_idx = li;
            opaque_count++;
        } else {
            keyed_idx = li;
            keyed_count++;
        }
    }
    const bool base_fused_has_override =
        (opaque_count == 1) && (c->layers[base_idx].scanline_override != NULL);
    const bool base_fused_eligible = base_fused_has_override && (c->palette_banks == NULL);

    /* Fused hot path for the common PPU-style case: one opaque base with
     * scanline_override + exactly one keyed overlay. Collapses
     * fetch + merge + palette + swap into two passes instead of three. */
    if (base_fused_has_override && keyed_count == 1) {
        const crt_compose_layer_t *base = &c->layers[base_idx];
        const crt_compose_layer_t *keyed = &c->layers[keyed_idx];

        if (active_width == CRT_COMPOSITE_RGB332_ACTIVE_WIDTH &&
            logical_width == CRT_COMPOSITE_RGB332_WIDTH &&
            keyed->fetch_attr == crt_sprite_layer_fetch_with_attrs) {
            layers_fetched++;
            base->scanline_override(scanline, active_buf, active_width, base->ctx);
            layers_fetched++;
            if (crt_compose_patch_sprite_spans_256(c, keyed, scanline->logical_line, active_buf)) {
                crt_compose_stats_record(c, false, layers_fetched);
                return;
            }
        }

        if (!base_fused_eligible) {
            goto generic_path;
        }

        memset(c->attr_scratch, 0, logical_width);
        layers_fetched++;
        if (!crt_compose_fetch_layer(keyed, scanline->logical_line, c->scratch, c->attr_scratch,
                                     logical_width)) {
            layers_fetched++;
            base->scanline_override(scanline, active_buf, active_width, base->ctx);
            crt_compose_stats_record(c, false, layers_fetched);
            return;
        }

        memset(c->attr_line, 0, logical_width);
        layers_fetched++;
        (void)crt_compose_fetch_layer(base, scanline->logical_line, c->line, c->attr_line,
                                      logical_width);

        const uint8_t key = (uint8_t)keyed->transparent_idx;
        crt_compose_merge_keyed_line(c, logical_width, key);
        crt_compose_render_palette_line(c, active_buf, active_width, logical_width);
        crt_compose_stats_record(c, true, layers_fetched);
        return;
    }

    if (base_fused_eligible) {
        /* Lazy materialization path.
         *
         * base.fetch is deferred until a keyed layer actually contributes.
         * If none does, we delegate the whole scanline to base.scanline_override
         * — bit-exact with the pre-compose hook, zero materialization cost.
         * If at least one keyed contributes, we materialize the base into
         * c->line, merge scratches into it, and finish with palette+swap.
         */
        const crt_compose_layer_t *base = &c->layers[base_idx];
        bool line_materialized = false;

        for (uint8_t li = 0; li < c->layer_count; ++li) {
            if ((int)li == base_idx) {
                continue;
            }
            const crt_compose_layer_t *layer = &c->layers[li];
            if (!layer->enabled || !crt_compose_layer_has_fetch(layer)) {
                continue;
            }

            memset(c->attr_scratch, 0, logical_width);
            layers_fetched++;
            if (!crt_compose_fetch_layer(layer, scanline->logical_line, c->scratch, c->attr_scratch,
                                         logical_width)) {
                continue;
            }

            if (!line_materialized) {
                memset(c->attr_line, 0, logical_width);
                layers_fetched++;
                (void)crt_compose_fetch_layer(base, scanline->logical_line, c->line, c->attr_line,
                                              logical_width);
                line_materialized = true;
            }

            const uint8_t key = (uint8_t)layer->transparent_idx;
            crt_compose_merge_keyed_line(c, logical_width, key);
        }

        if (!line_materialized) {
            layers_fetched++;
            base->scanline_override(scanline, active_buf, active_width, base->ctx);
            crt_compose_stats_record(c, false, layers_fetched);
            return;
        }
        /* else: fall through to the palette + word-swap pass below. */
    } else {
    generic_path:
        /* Generic path (no fused base): composite layers back-to-front.
         *  - Opaque layer writes directly into c->line, skipping memcpy.
         *    When the first enabled layer is opaque we skip the base clear.
         *  - Keyed layer returning false is skipped with no merge work.
         */
        bool line_ready = false;
        for (uint8_t li = 0; li < c->layer_count; ++li) {
            const crt_compose_layer_t *layer = &c->layers[li];
            if (!layer->enabled || !crt_compose_layer_has_fetch(layer)) {
                continue;
            }

            if (layer->transparent_idx == CRT_COMPOSE_NO_TRANSPARENCY) {
                memset(c->attr_line, 0, logical_width);
                layers_fetched++;
                (void)crt_compose_fetch_layer(layer, scanline->logical_line, c->line, c->attr_line,
                                              logical_width);
                line_ready = true;
                continue;
            }

            if (!line_ready) {
                memset(c->line, c->clear_idx, logical_width);
                memset(c->attr_line, 0, logical_width);
                line_ready = true;
            }

            memset(c->attr_scratch, 0, logical_width);
            layers_fetched++;
            if (!crt_compose_fetch_layer(layer, scanline->logical_line, c->scratch, c->attr_scratch,
                                         logical_width)) {
                continue;
            }

            const uint8_t key = (uint8_t)layer->transparent_idx;
            crt_compose_merge_keyed_line(c, logical_width, key);
        }

        if (!line_ready) {
            memset(c->line, c->clear_idx, logical_width);
            memset(c->attr_line, 0, logical_width);
        }
    }

    crt_compose_stats_record(c, true, layers_fetched);
    crt_compose_apply_palette_banks(c, logical_width);
    crt_compose_render_palette_line(c, active_buf, active_width, logical_width);
}

IRAM_ATTR void crt_compose_scanline_hook_rgb332_256(const crt_scanline_t *scanline,
                                                    uint16_t *active_buf, uint16_t active_width,
                                                    void *user_data)
{
    crt_compose_t *c = (crt_compose_t *)user_data;

    if (scanline == NULL || c == NULL || active_buf == NULL ||
        active_width != CRT_COMPOSITE_RGB332_ACTIVE_WIDTH || scanline->timing == NULL ||
        !CRT_SCANLINE_HAS_LOGICAL(scanline)) {
        return;
    }

    const uint8_t layers_fetched =
        crt_compose_render_indexed_line(c, scanline->logical_line, CRT_COMPOSITE_RGB332_WIDTH);
    crt_compose_stats_record(c, true, layers_fetched);
    crt_composite_rgb332_render_256_to_768(scanline->timing->standard, scanline->physical_line,
                                           c->line, active_buf);
}
