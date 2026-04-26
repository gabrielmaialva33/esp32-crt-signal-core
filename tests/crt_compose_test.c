/**
 * @file crt_compose_test.c
 * @brief Host-compiled test for crt_compose layer mixer.
 *
 * gcc -I components/crt_compose/include -I components/crt_core/include \
 *     -I components/crt_timing/include -I tests/stubs \
 *     tests/crt_compose_test.c components/crt_compose/crt_compose.c \
 *     -o /tmp/crt_compose_test && /tmp/crt_compose_test
 */

#include "crt_compose.h"
#include "crt_compose_layers.h"
#include "crt_sprite.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static uint16_t g_palette[256];
static uint8_t g_sprite_atlas[256 * 32];

static void init_linear_palette(void)
{
    for (int i = 0; i < 256; ++i) {
        g_palette[i] = (uint16_t)(i << 8);
    }
}

static void init_sprite_atlas(void) {
    memset(g_sprite_atlas, 0, sizeof(g_sprite_atlas));
    for (uint16_t y = 0; y < 32; ++y) {
        for (uint16_t x = 0; x < 256; ++x) {
            g_sprite_atlas[(y * 256U) + x] = (uint8_t)(1U + x + (y * 16U));
        }
    }
}

static crt_scanline_t make_active_line(uint16_t logical)
{
    static crt_timing_profile_t timing;
    memset(&timing, 0, sizeof(timing));
    timing.total_lines = 262;
    timing.active_lines = 240;
    return (crt_scanline_t)
    {
        .physical_line = logical + 20,
        .logical_line = logical,
        .type = CRT_LINE_ACTIVE,
        .field = 0,
        .frame_number = 0,
        .subcarrier_phase = 0,
        .timing = &timing,
    };
}

/* Fills line with a fixed pattern derived from logical_line + offset. */
typedef struct {
    uint8_t base;
    uint8_t step;
} pattern_ctx_t;

static bool pattern_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out, uint16_t width) {
    const pattern_ctx_t *p = (const pattern_ctx_t *)ctx;
    for (uint16_t x = 0; x < width; ++x) {
        idx_out[x] = (uint8_t)(p->base + (logical_line * p->step) + (uint8_t)x);
    }
    return true;
}

/* Writes a keyed sprite: every 3rd pixel is opaque, rest is key=0. */
typedef struct {
    uint8_t value;
    uint8_t key;
} sprite_ctx_t;

static bool sprite_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out, uint16_t width) {
    const sprite_ctx_t *s = (const sprite_ctx_t *)ctx;
    (void)logical_line;
    for (uint16_t x = 0; x < width; ++x) {
        idx_out[x] = (x % 3 == 0) ? s->value : s->key;
    }
    return true;
}

/* ── Fused-path mocks ─────────────────────────────────────────────── */

static uint32_t g_mock_base_fetch_calls;
static uint32_t g_mock_base_override_calls;
static uint32_t g_mock_absent_overlay_calls;

/* mock_base_fetch: writes `(x + y) & 0xFF` per pixel. */
static bool mock_base_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out, uint16_t width) {
    (void)ctx;
    g_mock_base_fetch_calls++;
    for (uint16_t x = 0; x < width; ++x) {
        idx_out[x] = (uint8_t)(x + logical_line);
    }
    return true;
}

/* mock_base_override: writes palette[(x + y) & 0xFF] with the same word-swap
 * as the generic compose output pass. Guarantees bit-exact parity with
 * mock_base_fetch + palette + swap. */
static void mock_base_override(const crt_scanline_t *scanline, uint16_t *active_buf,
                               uint16_t active_width, void *user_data) {
    (void)user_data;
    g_mock_base_override_calls++;
    const uint16_t *pal = g_palette;
    const uint16_t y = scanline->logical_line;
    const uint16_t even_width = active_width & (uint16_t)~1U;
    uint16_t i = 0;
    for (; i < even_width; i += 2) {
        uint16_t p0 = pal[(uint8_t)(i + y)];
        uint16_t p1 = pal[(uint8_t)(i + 1 + y)];
        active_buf[i] = p1;
        active_buf[i + 1] = p0;
    }
    if (i < active_width) {
        active_buf[i] = pal[(uint8_t)(i + y)];
    }
}

/* mock_absent_overlay_fetch: keyed layer that never contributes. Must NOT
 * touch idx_out; compose is required to skip merge when false is returned. */
static bool mock_absent_overlay_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                      uint16_t width) {
    (void) ctx;
    (void) logical_line;
    (void) idx_out;
    (void) width;
    g_mock_absent_overlay_calls++;
    return false;
}

static void reset_mock_counters(void)
{
    g_mock_base_fetch_calls = 0;
    g_mock_base_override_calls = 0;
    g_mock_absent_overlay_calls = 0;
}

/* Tracks how many times the fetch was called. */
typedef struct {
    uint32_t calls;
    uint8_t fill;
} counting_ctx_t;

static bool counting_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out, uint16_t width) {
    counting_ctx_t *c = (counting_ctx_t *)ctx;
    (void)logical_line;
    c->calls++;
    memset(idx_out, c->fill, width);
    return true;
}

/* ── Tests ────────────────────────────────────────────────────────── */

static void test_init_and_palette(void)
{
    crt_compose_t c;
    assert(crt_compose_init(&c) == 0);
    assert(c.layer_count == 0);
    assert(c.palette == NULL);

    init_linear_palette();
    assert(crt_compose_set_palette(&c, g_palette, 256) == 0);
    assert(c.palette == g_palette);
    assert(c.palette_size == 256);

    assert(crt_compose_set_palette(&c, g_palette, 255) == ESP_ERR_INVALID_SIZE);
    assert(c.palette == g_palette);
    assert(c.palette_size == 256);

    assert(crt_compose_set_palette(&c, NULL, 0) == 0);
    assert(c.palette == NULL);
    assert(c.palette_size == 0);

    assert(crt_compose_set_palette(&c, g_palette, 256) == 0);
    printf("  init/palette: OK\n");
}

static void test_add_layer_limits(void)
{
    crt_compose_t c;
    crt_compose_init(&c);

    pattern_ctx_t pc = {.base = 0, .step = 1};
    for (uint8_t i = 0; i < CRT_COMPOSE_MAX_LAYERS; ++i) {
        assert(crt_compose_add_layer(&c, pattern_fetch, &pc, CRT_COMPOSE_NO_TRANSPARENCY) == 0);
    }
    /* One past max should fail */
    assert(crt_compose_add_layer(&c, pattern_fetch, &pc, CRT_COMPOSE_NO_TRANSPARENCY) != 0);
    assert(c.layer_count == CRT_COMPOSE_MAX_LAYERS);

    crt_compose_clear_layers(&c);
    assert(c.layer_count == 0);
    printf("  add/clear layers: OK\n");
}

static void test_layer_ids_and_info(void) {
    crt_compose_t c;
    crt_compose_init(&c);

    uint8_t bg_id = CRT_COMPOSE_LAYER_INVALID;
    uint8_t overlay_id = CRT_COMPOSE_LAYER_INVALID;
    counting_ctx_t bg = {.calls = 0, .fill = 1};
    counting_ctx_t overlay = {.calls = 0, .fill = 2};

    assert(crt_compose_add_layer_with_id(&c, counting_fetch, &bg, CRT_COMPOSE_NO_TRANSPARENCY,
                                         &bg_id) == 0);
    assert(crt_compose_add_layer_with_id(&c, counting_fetch, &overlay, 0, &overlay_id) == 0);
    assert(bg_id == 0);
    assert(overlay_id == 1);

    crt_compose_layer_info_t info;
    assert(crt_compose_get_layer_info(&c, overlay_id, &info) == 0);
    assert(info.fetch == counting_fetch);
    assert(info.ctx == &overlay);
    assert(info.transparent_idx == 0);
    assert(info.enabled);
    assert(info.scanline_override == NULL);

    crt_compose_set_layer_enabled(&c, overlay_id, false);
    assert(crt_compose_get_layer_info(&c, overlay_id, &info) == 0);
    assert(!info.enabled);

    assert(crt_compose_get_layer_info(&c, 99, &info) == ESP_ERR_INVALID_ARG);
    printf("  layer ids + info: OK\n");
}

static void test_single_opaque_layer_with_swap(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    pattern_ctx_t pc = {.base = 10, .step = 0}; /* line = 10, 11, 12, 13, ... */
    assert(crt_compose_add_layer(&c, pattern_fetch, &pc, CRT_COMPOSE_NO_TRANSPARENCY) == 0);

    uint16_t active_buf[8] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 8, &c);

    /* Expected indexed line: 10,11,12,13,14,15,16,17
     * After word-swap (pairs):
     *   active_buf[0] = pal[11], active_buf[1] = pal[10]
     *   active_buf[2] = pal[13], active_buf[3] = pal[12]
     *   active_buf[4] = pal[15], active_buf[5] = pal[14]
     *   active_buf[6] = pal[17], active_buf[7] = pal[16]
     */
    assert(active_buf[0] == g_palette[11]);
    assert(active_buf[1] == g_palette[10]);
    assert(active_buf[2] == g_palette[13]);
    assert(active_buf[3] == g_palette[12]);
    assert(active_buf[4] == g_palette[15]);
    assert(active_buf[5] == g_palette[14]);
    assert(active_buf[6] == g_palette[17]);
    assert(active_buf[7] == g_palette[16]);
    printf("  single opaque + word-swap: OK\n");
}

static void test_odd_width_tail(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    pattern_ctx_t pc = {.base = 0, .step = 0}; /* 0,1,2 */
    crt_compose_add_layer(&c, pattern_fetch, &pc, CRT_COMPOSE_NO_TRANSPARENCY);

    uint16_t active_buf[3] = {0xDEAD, 0xDEAD, 0xDEAD};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 3, &c);

    assert(active_buf[0] == g_palette[1]);
    assert(active_buf[1] == g_palette[0]);
    assert(active_buf[2] == g_palette[2]); /* tail, un-swapped */
    printf("  odd-width tail: OK\n");
}

static void test_transparent_overlay(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    /* Layer 0: solid 50 (opaque) */
    counting_ctx_t bg = {.calls = 0, .fill = 50};
    crt_compose_add_layer(&c, counting_fetch, &bg, CRT_COMPOSE_NO_TRANSPARENCY);

    /* Layer 1: sprite value=99, key=0; pattern= 99,0,0,99,0,0,99,0 */
    sprite_ctx_t spr = {.value = 99, .key = 0};
    crt_compose_add_layer(&c, sprite_fetch, &spr, 0);

    uint16_t active_buf[8] = {0};
    crt_scanline_t sc = make_active_line(5);
    crt_compose_scanline_hook(&sc, active_buf, 8, &c);

    /* Composed indexed line: 99,50,50,99,50,50,99,50
     * After word-swap:
     *  [0]=pal[50], [1]=pal[99]
     *  [2]=pal[99], [3]=pal[50]
     *  [4]=pal[50], [5]=pal[50]
     *  [6]=pal[50], [7]=pal[99]
     */
    assert(active_buf[0] == g_palette[50]);
    assert(active_buf[1] == g_palette[99]);
    assert(active_buf[2] == g_palette[99]);
    assert(active_buf[3] == g_palette[50]);
    assert(active_buf[4] == g_palette[50]);
    assert(active_buf[5] == g_palette[50]);
    assert(active_buf[6] == g_palette[50]);
    assert(active_buf[7] == g_palette[99]);

    assert(bg.calls == 1);
    printf("  transparent overlay z-order: OK\n");
}

static void test_disabled_layer_skipped(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);
    crt_compose_set_clear_index(&c, 7);

    counting_ctx_t ctx = {.calls = 0, .fill = 200};
    crt_compose_add_layer(&c, counting_fetch, &ctx, CRT_COMPOSE_NO_TRANSPARENCY);
    crt_compose_set_layer_enabled(&c, 0, false);

    uint16_t active_buf[4] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);

    /* Disabled layer not called; line is clear_idx everywhere = 7 */
    assert(ctx.calls == 0);
    assert(active_buf[0] == g_palette[7]);
    assert(active_buf[1] == g_palette[7]);
    assert(active_buf[2] == g_palette[7]);
    assert(active_buf[3] == g_palette[7]);
    printf("  disabled layer + clear_idx: OK\n");
}

static bool absent_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out, uint16_t width) {
    uint32_t *calls = (uint32_t *)ctx;
    (void)logical_line;
    (void)idx_out;
    (void)width;
    (*calls)++;
    /* Deliberately do not touch idx_out: compose must not read it back. */
    return false;
}

static void test_keyed_absent_skips_merge(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    /* Layer 0: solid 77 (opaque BG) */
    counting_ctx_t bg = {.calls = 0, .fill = 77};
    crt_compose_add_layer(&c, counting_fetch, &bg, CRT_COMPOSE_NO_TRANSPARENCY);

    /* Layer 1: keyed overlay that reports "not present" */
    uint32_t absent_calls = 0;
    crt_compose_add_layer(&c, absent_fetch, &absent_calls, 0);

    uint16_t active_buf[4] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);

    /* Overlay was queried but its contents were ignored -> BG shows through. */
    assert(absent_calls == 1);
    assert(active_buf[0] == g_palette[77]);
    assert(active_buf[1] == g_palette[77]);
    assert(active_buf[2] == g_palette[77]);
    assert(active_buf[3] == g_palette[77]);
    printf("  keyed absent fetch skips merge: OK\n");
}

static void test_layer_context_and_fetch_mutation(void) {
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    uint8_t layer_id = CRT_COMPOSE_LAYER_INVALID;
    counting_ctx_t original = {.calls = 0, .fill = 10};
    counting_ctx_t replacement = {.calls = 0, .fill = 44};
    assert(crt_compose_add_layer_with_id(&c, counting_fetch, &original,
                                         CRT_COMPOSE_NO_TRANSPARENCY, &layer_id) == 0);
    assert(crt_compose_set_layer_context(&c, layer_id, &replacement) == 0);

    uint16_t active_buf[4] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);
    assert(original.calls == 0);
    assert(replacement.calls == 1);
    assert(active_buf[0] == g_palette[44]);
    assert(active_buf[1] == g_palette[44]);

    counting_ctx_t second = {.calls = 0, .fill = 88};
    assert(crt_compose_set_layer_fetch(&c, layer_id, counting_fetch, &second) == 0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);
    assert(second.calls == 1);
    assert(active_buf[0] == g_palette[88]);
    assert(active_buf[1] == g_palette[88]);

    assert(crt_compose_set_layer_fetch(&c, layer_id, NULL, &second) == ESP_ERR_INVALID_ARG);
    assert(crt_compose_set_layer_context(&c, 99, &second) == ESP_ERR_INVALID_ARG);
    printf("  layer context/fetch mutation: OK\n");
}

static void test_layer_transparency_mutation(void) {
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    counting_ctx_t bg = {.calls = 0, .fill = 50};
    sprite_ctx_t spr = {.value = 99, .key = 0};
    uint8_t overlay_id = CRT_COMPOSE_LAYER_INVALID;
    crt_compose_add_layer(&c, counting_fetch, &bg, CRT_COMPOSE_NO_TRANSPARENCY);
    crt_compose_add_layer_with_id(&c, sprite_fetch, &spr, 0, &overlay_id);

    uint16_t active_buf[4] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[50]);
    assert(active_buf[1] == g_palette[99]);

    assert(crt_compose_set_layer_transparent_index(&c, overlay_id, 99) == 0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[0]);
    assert(active_buf[1] == g_palette[50]);

    assert(crt_compose_set_layer_transparent_index(&c, 99, 0) == ESP_ERR_INVALID_ARG);
    printf("  layer transparency mutation: OK\n");
}

static void test_builtin_solid_layer(void) {
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    crt_compose_solid_layer_t solid;
    crt_compose_solid_layer_init(&solid, 12);
    crt_compose_add_layer(&c, crt_compose_solid_layer_fetch, &solid,
                          CRT_COMPOSE_NO_TRANSPARENCY);

    uint16_t active_buf[4] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);

    assert(active_buf[0] == g_palette[12]);
    assert(active_buf[1] == g_palette[12]);
    assert(active_buf[2] == g_palette[12]);
    assert(active_buf[3] == g_palette[12]);
    printf("  builtin solid layer: OK\n");
}

static void test_builtin_rect_layer_overlay(void) {
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    crt_compose_solid_layer_t bg;
    crt_compose_solid_layer_init(&bg, 10);
    crt_compose_add_layer(&c, crt_compose_solid_layer_fetch, &bg, CRT_COMPOSE_NO_TRANSPARENCY);

    crt_compose_rect_layer_t rect;
    crt_compose_rect_layer_init(&rect, 1, 2, 2, 1, 77, 0);
    crt_compose_add_layer(&c, crt_compose_rect_layer_fetch, &rect, 0);

    uint16_t active_buf[4] = {0};
    crt_scanline_t miss = make_active_line(1);
    crt_compose_scanline_hook(&miss, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[10]);
    assert(active_buf[1] == g_palette[10]);

    crt_scanline_t hit = make_active_line(2);
    crt_compose_scanline_hook(&hit, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[77]);
    assert(active_buf[1] == g_palette[10]);
    assert(active_buf[2] == g_palette[10]);
    assert(active_buf[3] == g_palette[77]);

    crt_compose_rect_layer_set_bounds(&rect, 0, 2, 4, 1);
    crt_compose_rect_layer_set_fill(&rect, 88);
    crt_compose_scanline_hook(&hit, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[88]);
    assert(active_buf[1] == g_palette[88]);
    assert(active_buf[2] == g_palette[88]);
    assert(active_buf[3] == g_palette[88]);
    printf("  builtin rect overlay: OK\n");
}

static void test_builtin_checker_layer(void) {
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    crt_compose_checker_layer_t checker;
    crt_compose_checker_layer_init(&checker, 3, 4, 2, 1);
    crt_compose_add_layer(&c, crt_compose_checker_layer_fetch, &checker,
                          CRT_COMPOSE_NO_TRANSPARENCY);

    uint16_t active_buf[4] = {0};
    crt_scanline_t line0 = make_active_line(0);
    crt_compose_scanline_hook(&line0, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[3]);
    assert(active_buf[1] == g_palette[3]);
    assert(active_buf[2] == g_palette[4]);
    assert(active_buf[3] == g_palette[4]);

    crt_scanline_t line1 = make_active_line(1);
    crt_compose_scanline_hook(&line1, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[4]);
    assert(active_buf[1] == g_palette[4]);
    assert(active_buf[2] == g_palette[3]);
    assert(active_buf[3] == g_palette[3]);
    printf("  builtin checker layer: OK\n");
}

static void test_builtin_viewport_layer_scrolls_and_clips(void) {
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    crt_compose_solid_layer_t bg;
    crt_compose_solid_layer_init(&bg, 1);
    crt_compose_add_layer(&c, crt_compose_solid_layer_fetch, &bg, CRT_COMPOSE_NO_TRANSPARENCY);

    pattern_ctx_t source = {.base = 10, .step = 16};
    crt_compose_viewport_layer_t viewport;
    crt_compose_viewport_layer_init(&viewport, pattern_fetch, &source, 4, 4, 0);
    crt_compose_viewport_layer_set_viewport(&viewport, 1, 1, 3, 2);
    crt_compose_viewport_layer_set_scroll(&viewport, 1, 0);
    crt_compose_add_layer(&c, crt_compose_viewport_layer_fetch, &viewport, 0);

    uint16_t active_buf[4] = {0};
    crt_scanline_t outside = make_active_line(0);
    crt_compose_scanline_hook(&outside, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[1]);
    assert(active_buf[1] == g_palette[1]);
    assert(active_buf[2] == g_palette[1]);
    assert(active_buf[3] == g_palette[1]);

    crt_scanline_t hit = make_active_line(1);
    crt_compose_scanline_hook(&hit, active_buf, 4, &c);
    /* Source line 0 is 10,11,12,13. Viewport starts at x=1 and scroll_x=1,
     * so the composed indexed line is 1,11,12,13 before word swap. */
    assert(active_buf[0] == g_palette[11]);
    assert(active_buf[1] == g_palette[1]);
    assert(active_buf[2] == g_palette[13]);
    assert(active_buf[3] == g_palette[12]);

    crt_compose_viewport_layer_set_scroll(&viewport, 0, 3);
    crt_compose_scanline_hook(&hit, active_buf, 4, &c);
    /* Vertical scroll wraps to source line 3: 58,59,60,61.
     * Viewport writes first three samples into x=1..3. */
    assert(active_buf[0] == g_palette[58]);
    assert(active_buf[1] == g_palette[1]);
    assert(active_buf[2] == g_palette[60]);
    assert(active_buf[3] == g_palette[59]);

    crt_compose_viewport_layer_scroll_by(&viewport, 1, 1);
    crt_compose_scanline_hook(&hit, active_buf, 4, &c);
    /* Scroll is now x=1,y=4; y wraps to source line 0 again. */
    assert(active_buf[0] == g_palette[11]);
    assert(active_buf[1] == g_palette[1]);
    assert(active_buf[2] == g_palette[13]);
    assert(active_buf[3] == g_palette[12]);
    printf("  builtin viewport scroll/clip layer: OK\n");
}

static void test_sprite_atlas_and_basic_fetch(void) {
    init_sprite_atlas();

    crt_sprite_atlas_t atlas;
    crt_sprite_layer_t sprites;
    uint8_t sprite_id = CRT_SPRITE_INVALID_ID;

    assert(crt_sprite_atlas_init(&atlas, g_sprite_atlas, 256, 32, 256) == 0);
    assert(crt_sprite_layer_init(&sprites, &atlas, 0) == 0);
    assert(crt_sprite_add(&sprites, 0, 0, CRT_SPRITE_SIZE_8X8, 1, 2, &sprite_id) == 0);
    assert(sprite_id == 0);

    /* Sprites with no Y match return false WITHOUT touching idx_out:
     * compose contract forbids the caller from reading idx_out when the
     * fetch returned false. Skipping the memset is what keeps the demo
     * at 0 underruns. */
    uint8_t line[12];
    memset(line, 0xEE, sizeof(line));
    assert(!crt_sprite_layer_fetch(&sprites, 1, line, 12));
    for (uint8_t i = 0; i < 12; ++i) {
        assert(line[i] == 0xEE);
    }

    memset(line, 0xEE, sizeof(line));
    assert(crt_sprite_layer_fetch(&sprites, 2, line, 12));
    assert(line[0] == 0);
    assert(line[1] == 1);
    assert(line[2] == 2);
    assert(line[8] == 8);
    assert(line[9] == 0);
    assert(sprites.last_line_considered == 1);
    assert(sprites.last_line_rendered == 1);
    assert(sprites.last_line_overflow == 0);
    printf("  sprite atlas + basic fetch: OK\n");
}

static void test_sprite_position_frame_size_and_scale(void) {
    init_sprite_atlas();

    crt_sprite_atlas_t atlas;
    crt_sprite_layer_t sprites;
    uint8_t sprite_id = CRT_SPRITE_INVALID_ID;
    crt_sprite_t sprite;

    crt_sprite_atlas_init(&atlas, g_sprite_atlas, 256, 32, 256);
    crt_sprite_layer_init(&sprites, &atlas, 0);
    crt_sprite_layer_set_x_scale(&sprites, 3);
    assert(crt_sprite_add(&sprites, 1, 1, CRT_SPRITE_SIZE_16X16, 2, 4, &sprite_id) == 0);

    assert(crt_sprite_set_position(&sprites, sprite_id, -1, 4) == 0);
    assert(crt_sprite_move_by(&sprites, sprite_id, 2, 1) == 0);
    assert(crt_sprite_set_frame(&sprites, sprite_id, 2) == 0);
    assert(crt_sprite_get(&sprites, sprite_id, &sprite) == 0);
    assert(sprite.x == 1);
    assert(sprite.y == 5);
    assert(sprite.cell_x == 2);
    assert(sprite.cell_y == 1);
    assert(sprite.size == CRT_SPRITE_SIZE_16X16);

    uint8_t line[32] = {0};
    assert(crt_sprite_layer_fetch(&sprites, 5, line, 32));
    assert(line[0] == 0);
    assert(line[3] == 145);
    assert(line[4] == 145);
    assert(line[5] == 145);
    assert(line[6] == 146);

    assert(crt_sprite_set_size(&sprites, sprite_id, CRT_SPRITE_SIZE_32X32) ==
           ESP_ERR_INVALID_ARG);
    assert(crt_sprite_set_enabled(&sprites, sprite_id, false) == 0);
    assert(!crt_sprite_layer_fetch(&sprites, 5, line, 32));
    printf("  sprite position/frame/size/scale: OK\n");
}

static void test_sprite_per_line_cap_and_overflow(void) {
    init_sprite_atlas();

    crt_sprite_atlas_t atlas;
    crt_sprite_layer_t sprites;

    crt_sprite_atlas_init(&atlas, g_sprite_atlas, 256, 32, 256);
    crt_sprite_layer_init(&sprites, &atlas, 0);
    crt_sprite_layer_set_max_sprites_per_line(&sprites, 2);

    for (uint8_t i = 0; i < 4; ++i) {
        uint8_t id = CRT_SPRITE_INVALID_ID;
        assert(crt_sprite_add(&sprites, i, 0, CRT_SPRITE_SIZE_8X8, (int16_t)(i * 2), 0,
                              &id) == 0);
        assert(id == i);
    }

    uint8_t line[32] = {0};
    assert(crt_sprite_layer_fetch(&sprites, 0, line, 32));
    assert(sprites.last_line_considered == 4);
    assert(sprites.last_line_rendered == 2);
    assert(sprites.last_line_overflow == 2);
    assert(sprites.overflow_count == 2);

    crt_sprite_layer_reset_stats(&sprites);
    assert(sprites.overflow_count == 0);
    printf("  sprite per-line cap + overflow: OK\n");
}

static void test_opaque_base_skips_clear(void)
{
    /* Ensure opaque layer 0 fully controls the line content regardless of
     * clear_idx (i.e. compose does not pre-fill and then overwrite). */
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);
    crt_compose_set_clear_index(&c, 33); /* would be visible if clear happened */

    counting_ctx_t bg = {.calls = 0, .fill = 101};
    crt_compose_add_layer(&c, counting_fetch, &bg, CRT_COMPOSE_NO_TRANSPARENCY);

    uint16_t active_buf[4] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);

    /* Every pixel should be 101 (from layer), never 33 (clear_idx). */
    assert(active_buf[0] == g_palette[101]);
    assert(active_buf[1] == g_palette[101]);
    assert(active_buf[2] == g_palette[101]);
    assert(active_buf[3] == g_palette[101]);
    assert(bg.calls == 1);
    printf("  opaque base bypasses clear: OK\n");
}

static void test_fused_base_solo_delegates(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);
    reset_mock_counters();

    assert(crt_compose_add_layer_fused(&c, mock_base_fetch, mock_base_override, NULL) == 0);

    uint16_t buf[8] = {0};
    crt_scanline_t sc = make_active_line(3);
    crt_compose_scanline_hook(&sc, buf, 8, &c);

    /* Only the override runs; generic fetch path must stay silent. */
    assert(g_mock_base_override_calls == 1);
    assert(g_mock_base_fetch_calls == 0);

    /* Output produced by the override (pal[(x+y)&0xFF] with word-swap). */
    assert(buf[0] == g_palette[4]);
    assert(buf[1] == g_palette[3]);
    assert(buf[2] == g_palette[6]);
    assert(buf[3] == g_palette[5]);
    assert(buf[4] == g_palette[8]);
    assert(buf[5] == g_palette[7]);
    assert(buf[6] == g_palette[10]);
    assert(buf[7] == g_palette[9]);
    printf("  fused base solo delegates direct: OK\n");
}

static void test_fused_base_plus_absent_overlay_delegates(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);
    reset_mock_counters();

    crt_compose_add_layer_fused(&c, mock_base_fetch, mock_base_override, NULL);
    crt_compose_add_layer(&c, mock_absent_overlay_fetch, NULL, 0);

    uint16_t buf[4] = {0};
    crt_scanline_t sc = make_active_line(5);
    crt_compose_scanline_hook(&sc, buf, 4, &c);

    /* Overlay must be probed exactly once.
     * Nothing contributes -> delegation wins, no materialization. */
    assert(g_mock_absent_overlay_calls == 1);
    assert(g_mock_base_override_calls == 1);
    assert(g_mock_base_fetch_calls == 0);

    /* Output matches what the override writes, not the generic path. */
    assert(buf[0] == g_palette[(uint8_t)(1 + 5)]);
    assert(buf[1] == g_palette[(uint8_t)(0 + 5)]);
    assert(buf[2] == g_palette[(uint8_t)(3 + 5)]);
    assert(buf[3] == g_palette[(uint8_t)(2 + 5)]);
    printf("  fused base + absent overlay still delegates: OK\n");
}

static void test_fused_base_plus_present_overlay_materializes(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);
    reset_mock_counters();

    crt_compose_add_layer_fused(&c, mock_base_fetch, mock_base_override, NULL);
    counting_ctx_t overlay = {.calls = 0, .fill = 99};
    crt_compose_add_layer(&c, counting_fetch, &overlay, 0);

    uint16_t buf[4] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, buf, 4, &c);

    /* Overlay contributed -> base materializes, override is NOT called. */
    assert(overlay.calls == 1);
    assert(g_mock_base_fetch_calls == 1);
    assert(g_mock_base_override_calls == 0);

    /* Composed line: overlay wrote 99 everywhere (no transparent pixels),
     * so every slot = 99, then palette+swap. */
    assert(buf[0] == g_palette[99]);
    assert(buf[1] == g_palette[99]);
    assert(buf[2] == g_palette[99]);
    assert(buf[3] == g_palette[99]);
    printf("  fused base + present overlay materializes: OK\n");
}

static void test_fused_vs_generic_parity(void)
{
    /* Same logical content rendered via the override path and via the
     * generic fetch+palette+swap path must produce identical active_buf. */
    init_linear_palette();
    crt_scanline_t sc = make_active_line(7);

    uint16_t buf_fused[16] = {0};
    uint16_t buf_generic[16] = {0};

    {
        crt_compose_t c;
        crt_compose_init(&c);
        crt_compose_set_palette(&c, g_palette, 256);
        crt_compose_add_layer_fused(&c, mock_base_fetch, mock_base_override, NULL);
        crt_compose_scanline_hook(&sc, buf_fused, 16, &c);
    }

    {
        crt_compose_t c;
        crt_compose_init(&c);
        crt_compose_set_palette(&c, g_palette, 256);
        /* No override -> compose takes the generic path. */
        crt_compose_add_layer(&c, mock_base_fetch, NULL, CRT_COMPOSE_NO_TRANSPARENCY);
        crt_compose_scanline_hook(&sc, buf_generic, 16, &c);
    }

    for (int i = 0; i < 16; ++i) {
        assert(buf_fused[i] == buf_generic[i]);
    }
    printf("  fused override parity with generic path: OK\n");
}

static void test_swap_layers_changes_priority(void) {
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    counting_ctx_t back = {.calls = 0, .fill = 10};
    counting_ctx_t front = {.calls = 0, .fill = 20};
    uint8_t back_id = CRT_COMPOSE_LAYER_INVALID;
    uint8_t front_id = CRT_COMPOSE_LAYER_INVALID;

    crt_compose_add_layer_with_id(&c, counting_fetch, &back, CRT_COMPOSE_NO_TRANSPARENCY,
                                  &back_id);
    crt_compose_add_layer_with_id(&c, counting_fetch, &front, CRT_COMPOSE_NO_TRANSPARENCY,
                                  &front_id);

    uint16_t active_buf[4] = {0};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[20]);
    assert(active_buf[1] == g_palette[20]);

    assert(crt_compose_swap_layers(&c, back_id, front_id) == 0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);
    assert(active_buf[0] == g_palette[10]);
    assert(active_buf[1] == g_palette[10]);

    assert(crt_compose_swap_layers(&c, back_id, back_id) == 0);
    assert(crt_compose_swap_layers(&c, 99, front_id) == ESP_ERR_INVALID_ARG);
    assert(crt_compose_swap_layers(&c, back_id, 99) == ESP_ERR_INVALID_ARG);
    printf("  layer priority swap: OK\n");
}

static void test_non_active_line_noop(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    counting_ctx_t ctx = {.calls = 0, .fill = 1};
    crt_compose_add_layer(&c, counting_fetch, &ctx, CRT_COMPOSE_NO_TRANSPARENCY);

    uint16_t active_buf[4] = {0xBEEF, 0xBEEF, 0xBEEF, 0xBEEF};
    crt_scanline_t blank = {
        .physical_line = 250,
        .logical_line = CRT_SCANLINE_LOGICAL_LINE_NONE,
        .type = CRT_LINE_BLANK,
        .timing = NULL,
    };
    crt_compose_scanline_hook(&blank, active_buf, 4, &c);

    assert(ctx.calls == 0);
    assert(active_buf[0] == 0xBEEF);
    printf("  non-active line is no-op: OK\n");
}

static void test_missing_palette_noop(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    /* No palette set */

    counting_ctx_t ctx = {.calls = 0, .fill = 1};
    crt_compose_add_layer(&c, counting_fetch, &ctx, CRT_COMPOSE_NO_TRANSPARENCY);

    uint16_t active_buf[4] = {0xCAFE, 0xCAFE, 0xCAFE, 0xCAFE};
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, active_buf, 4, &c);

    assert(ctx.calls == 0);
    assert(active_buf[0] == 0xCAFE);
    printf("  missing palette is no-op: OK\n");
}

static void test_width_overflow_guarded(void)
{
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    counting_ctx_t ctx = {.calls = 0, .fill = 1};
    crt_compose_add_layer(&c, counting_fetch, &ctx, CRT_COMPOSE_NO_TRANSPARENCY);

    /* active_width > MAX must not invoke fetch or touch buffer */
    uint16_t buf = 0x1234;
    crt_scanline_t sc = make_active_line(0);
    crt_compose_scanline_hook(&sc, &buf, CRT_COMPOSE_MAX_WIDTH + 1, &c);

    assert(ctx.calls == 0);
    assert(buf == 0x1234);
    printf("  oversize width guarded: OK\n");
}

static void test_invalid_hook_inputs_noop(void) {
    crt_compose_t c;
    crt_compose_init(&c);
    init_linear_palette();
    crt_compose_set_palette(&c, g_palette, 256);

    counting_ctx_t ctx = {.calls = 0, .fill = 1};
    crt_compose_add_layer(&c, counting_fetch, &ctx, CRT_COMPOSE_NO_TRANSPARENCY);

    crt_scanline_t sc = make_active_line(0);
    uint16_t buf[4] = {0xBEEF, 0xBEEF, 0xBEEF, 0xBEEF};
    crt_compose_scanline_hook(NULL, buf, 4, &c);
    crt_compose_scanline_hook(&sc, NULL, 4, &c);
    crt_compose_scanline_hook(&sc, buf, 0, &c);
    crt_compose_scanline_hook(&sc, buf, 4, NULL);

    assert(ctx.calls == 0);
    assert(buf[0] == 0xBEEF);
    printf("  invalid hook inputs are no-op: OK\n");
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void)
{
    printf("crt_compose test\n");
    test_init_and_palette();
    test_add_layer_limits();
    test_layer_ids_and_info();
    test_single_opaque_layer_with_swap();
    test_odd_width_tail();
    test_transparent_overlay();
    test_disabled_layer_skipped();
    test_keyed_absent_skips_merge();
    test_layer_context_and_fetch_mutation();
    test_layer_transparency_mutation();
    test_builtin_solid_layer();
    test_builtin_rect_layer_overlay();
    test_builtin_checker_layer();
    test_builtin_viewport_layer_scrolls_and_clips();
    test_sprite_atlas_and_basic_fetch();
    test_sprite_position_frame_size_and_scale();
    test_sprite_per_line_cap_and_overflow();
    test_opaque_base_skips_clear();
    test_fused_base_solo_delegates();
    test_fused_base_plus_absent_overlay_delegates();
    test_fused_base_plus_present_overlay_materializes();
    test_fused_vs_generic_parity();
    test_swap_layers_changes_priority();
    test_non_active_line_noop();
    test_missing_palette_noop();
    test_width_overflow_guarded();
    test_invalid_hook_inputs_noop();
    printf("ALL PASSED\n");
    return 0;
}
