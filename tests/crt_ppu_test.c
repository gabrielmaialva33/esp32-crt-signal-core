#include "crt_composite_palette.h"
#include "crt_ppu.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t g_palette[256];
static uint8_t g_patterns[CRT_TILE_BYTES];
static uint8_t g_nametable[32 * 32];
static uint8_t g_attrs[32 * 32];
static uint8_t g_nes_attrs[8 * 8];
static uint8_t g_sprite_pixels[CRT_SPRITE_CELL_SIZE * CRT_SPRITE_CELL_SIZE];
static uint8_t g_bank1[256];
static crt_compose_palette_banks_t g_banks;

static void init_palette(void)
{
    for (int i = 0; i < 256; ++i) {
        g_palette[i] = (uint16_t)(i << 8);
        g_bank1[i] = (uint8_t)i;
    }
    g_bank1[0x10] = 0x22;
    g_bank1[0x11] = 0x33;
    memset(&g_banks, 0, sizeof(g_banks));
    g_banks.banks[1] = g_bank1;
}

static crt_scanline_t make_active_line(uint16_t logical)
{
    static crt_timing_profile_t timing;
    memset(&timing, 0, sizeof(timing));
    timing.standard = CRT_VIDEO_STANDARD_NTSC;
    timing.total_lines = 262;
    timing.active_lines = 240;
    return (crt_scanline_t){
        .physical_line = (uint16_t)(logical + 20),
        .logical_line = logical,
        .type = CRT_LINE_ACTIVE,
        .timing = &timing,
    };
}

static uint8_t attr_bank(uint8_t attr)
{
    return (uint8_t)((attr & CRT_TILE_ATTR_PALETTE_MASK) >> CRT_TILE_ATTR_PALETTE_SHIFT);
}

static void init_assets(crt_sprite_atlas_t *atlas)
{
    memset(g_patterns, 0x10, sizeof(g_patterns));
    memset(g_nametable, 0, sizeof(g_nametable));
    memset(g_attrs, 0, sizeof(g_attrs));
    memset(g_nes_attrs, 0, sizeof(g_nes_attrs));
    memset(g_sprite_pixels, 0, sizeof(g_sprite_pixels));
    g_attrs[0] = (uint8_t)(1u << CRT_TILE_ATTR_PALETTE_SHIFT);
    g_sprite_pixels[0] = 0x11;
    assert(crt_sprite_atlas_init(atlas, g_sprite_pixels, CRT_SPRITE_CELL_SIZE, CRT_SPRITE_CELL_SIZE,
                                 CRT_SPRITE_CELL_SIZE) == 0);
}

static crt_ppu_config_t make_config(const crt_sprite_atlas_t *atlas)
{
    return (crt_ppu_config_t){
        .visible_w_tiles = 32,
        .visible_h_tiles = 30,
        .pitch_w_tiles = 32,
        .pitch_h_tiles = 32,
        .pattern_table = g_patterns,
        .pattern_count = 1,
        .nametable = g_nametable,
        .attributes = g_attrs,
        .sprite_atlas = atlas,
        .sprite_transparent_idx = 0,
        .max_sprites_per_line = 4,
        .palette = g_palette,
        .palette_size = 256,
        .palette_banks = &g_banks,
    };
}

static void test_init_validation(void)
{
    crt_ppu_t ppu;
    crt_sprite_atlas_t atlas;
    init_palette();
    init_assets(&atlas);
    crt_ppu_config_t config = make_config(&atlas);

    assert(crt_ppu_init(NULL, &config) == ESP_ERR_INVALID_ARG);
    assert(crt_ppu_init(&ppu, NULL) == ESP_ERR_INVALID_ARG);
    config.sprite_atlas = NULL;
    assert(crt_ppu_init(&ppu, &config) == ESP_ERR_INVALID_ARG);
    printf("  init validation: OK\n");
}

static void test_nes_attribute_expansion(void)
{
    crt_ppu_t ppu;
    crt_sprite_atlas_t atlas;
    init_palette();
    init_assets(&atlas);
    crt_ppu_config_t config = make_config(&atlas);
    assert(crt_ppu_init(&ppu, &config) == 0);

    memset(g_attrs, 0, sizeof(g_attrs));
    g_attrs[0] = CRT_TILE_ATTR_PRIORITY;
    g_attrs[3] = CRT_TILE_ATTR_HFLIP;
    g_nes_attrs[0] = 0xE4; /* TL=0, TR=1, BL=2, BR=3. */

    assert(crt_ppu_load_nes_attributes(NULL, g_attrs, g_nes_attrs, 8, CRT_TILE_ATTR_PRIORITY) ==
           ESP_ERR_INVALID_ARG);
    assert(crt_ppu_load_nes_attributes(&ppu, NULL, g_nes_attrs, 8, CRT_TILE_ATTR_PRIORITY) ==
           ESP_ERR_INVALID_ARG);
    assert(crt_ppu_load_nes_attributes(&ppu, g_attrs, NULL, 8, CRT_TILE_ATTR_PRIORITY) ==
           ESP_ERR_INVALID_ARG);
    assert(crt_ppu_load_nes_attributes(&ppu, g_attrs, g_nes_attrs, 7, CRT_TILE_ATTR_PRIORITY) ==
           ESP_ERR_INVALID_ARG);

    assert(crt_ppu_load_nes_attributes(&ppu, g_attrs, g_nes_attrs, 8,
                                       CRT_TILE_ATTR_PRIORITY | CRT_TILE_ATTR_HFLIP) == ESP_OK);
    assert((crt_ppu_get_attr(&ppu, 0, 0) & CRT_TILE_ATTR_PRIORITY) != 0);
    assert(attr_bank(crt_ppu_get_attr(&ppu, 0, 0)) == 0);
    assert(attr_bank(crt_ppu_get_attr(&ppu, 2, 0)) == 1);
    assert((crt_ppu_get_attr(&ppu, 3, 0) & CRT_TILE_ATTR_HFLIP) != 0);
    assert(attr_bank(crt_ppu_get_attr(&ppu, 0, 2)) == 2);
    assert(attr_bank(crt_ppu_get_attr(&ppu, 2, 2)) == 3);
    printf("  NES attribute table expansion: OK\n");
}

static void test_tile_attr_sprite_and_hooks(void)
{
    crt_ppu_t ppu;
    crt_sprite_atlas_t atlas;
    init_palette();
    init_assets(&atlas);
    crt_ppu_config_t config = make_config(&atlas);
    assert(crt_ppu_init(&ppu, &config) == 0);

    assert(ppu.tile_layer_id == 0);
    assert(ppu.sprite_layer_id == 1);
    assert(crt_ppu_get_tile(&ppu, 0, 0) == 0);
    crt_ppu_set_attr(&ppu, 1, 0, (uint8_t)(1u << CRT_TILE_ATTR_PALETTE_SHIFT));
    assert(crt_ppu_get_attr(&ppu, 1, 0) == (uint8_t)(1u << CRT_TILE_ATTR_PALETTE_SHIFT));

    uint8_t sprite_id = CRT_SPRITE_INVALID_ID;
    assert(crt_ppu_add_sprite(&ppu, 0, 0, CRT_SPRITE_SIZE_8X8, 2, 0,
                              (uint8_t)(1u << CRT_SPRITE_ATTR_PALETTE_SHIFT), &sprite_id) == 0);
    assert(sprite_id == 0);
    assert(crt_sprite_get_attr(&ppu.sprites, sprite_id) ==
           (uint8_t)(1u << CRT_SPRITE_ATTR_PALETTE_SHIFT));

    crt_scanline_t sc = make_active_line(0);
    uint8_t expected_logical[CRT_COMPOSITE_RGB332_WIDTH];
    uint16_t expected_rgb[CRT_COMPOSITE_RGB332_ACTIVE_WIDTH];
    uint16_t actual_rgb[CRT_COMPOSITE_RGB332_ACTIVE_WIDTH] = {0};
    memset(expected_logical, 0x10, sizeof(expected_logical));
    for (uint16_t x = 0; x < CRT_TILE_PX_W; ++x) {
        expected_logical[x] = 0x22;
    }
    for (uint16_t x = CRT_TILE_PX_W; x < (uint16_t)(CRT_TILE_PX_W * 2u); ++x) {
        expected_logical[x] = 0x22;
    }
    expected_logical[2] = 0x33;
    crt_composite_rgb332_render_256_to_768(CRT_VIDEO_STANDARD_NTSC, sc.physical_line,
                                           expected_logical, expected_rgb);

    crt_ppu_scanline_hook_rgb332_256(&sc, actual_rgb, CRT_COMPOSITE_RGB332_ACTIVE_WIDTH, &ppu);
    assert(memcmp(actual_rgb, expected_rgb, sizeof(actual_rgb)) == 0);

    uint16_t pal_buf[CRT_COMPOSITE_RGB332_WIDTH] = {0};
    crt_ppu_scanline_hook(&sc, pal_buf, CRT_COMPOSITE_RGB332_WIDTH, &ppu);
    assert(pal_buf[0] == g_palette[0x22]);
    assert(pal_buf[1] == g_palette[0x22]);
    assert(pal_buf[2] == g_palette[0x22]);
    assert(pal_buf[3] == g_palette[0x33]);

    assert(crt_ppu_move_sprite_by(&ppu, sprite_id, 1, 2) == 0);
    crt_sprite_t sprite;
    assert(crt_sprite_get(&ppu.sprites, sprite_id, &sprite) == 0);
    assert(sprite.x == 3);
    assert(sprite.y == 2);
    assert(crt_ppu_set_sprite_position(&ppu, sprite_id, 4, 5) == 0);
    assert(crt_ppu_set_sprite_attr(&ppu, sprite_id, CRT_SPRITE_ATTR_HFLIP) == 0);
    assert(crt_sprite_get_attr(&ppu.sprites, sprite_id) == CRT_SPRITE_ATTR_HFLIP);
    printf("  tile attr + sprite facade + hooks: OK\n");
}

int main(void)
{
    printf("crt_ppu test\n");
    test_init_validation();
    test_nes_attribute_expansion();
    test_tile_attr_sprite_and_hooks();
    printf("ALL PASSED\n");
    return 0;
}
