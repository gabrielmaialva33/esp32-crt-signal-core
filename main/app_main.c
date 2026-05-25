#include "crt_compose.h"
#include "crt_compose_layers.h"
#include "crt_core.h"
#include "crt_demo_pattern.h"
#include "crt_fb.h"
#include "crt_ppu.h"
#include "crt_sprite.h"
#include "crt_stimulus.h"
#include "crt_tile.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "godzilla_img.h"
#include "sdkconfig.h"
#include "tile_demo.h"

#ifndef CONFIG_CRT_COMPOSE_STRESS_DEMO
#define CONFIG_CRT_COMPOSE_STRESS_DEMO 0
#endif

#define APP_DIAG_INTERVAL_MS 1000U

#if CONFIG_CRT_ENABLE_UART_UPLOAD
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include <fcntl.h>
#include <unistd.h>
#endif

static const char *TAG = "app_main";
static const bool k_enable_color = CONFIG_CRT_ENABLE_COLOR;
#if CONFIG_CRT_RENDER_MODE_RGB332_FB
static const bool k_use_rgb332_framebuffer = CONFIG_CRT_RENDER_MODE_RGB332_FB;
#else
static const bool k_use_rgb332_framebuffer = false;
#endif
#if CONFIG_CRT_RENDER_MODE_CALIBRATION
static const bool k_use_calibration_card = CONFIG_CRT_RENDER_MODE_CALIBRATION;
#else
static const bool k_use_calibration_card = false;
#endif
#if CONFIG_CRT_RENDER_MODE_RGB332_COMPOSE
static const bool k_use_rgb332_compose = CONFIG_CRT_RENDER_MODE_RGB332_COMPOSE;
#else
static const bool k_use_rgb332_compose = false;
#endif
#if CONFIG_CRT_RENDER_MODE_STIMULUS
static const bool k_use_stimulus = CONFIG_CRT_RENDER_MODE_STIMULUS;
#else
static const bool k_use_stimulus = false;
#endif
static crt_fb_surface_t s_fb;
static crt_ppu_t s_ppu;
static crt_stimulus_t s_stimulus;
static crt_compose_t s_stimulus_compose;
static crt_timing_profile_t s_app_timing;
static crt_timing_standard_info_t s_app_standard_info;
static bool s_app_metadata_valid = false;
static uint32_t s_last_diag_underruns = 0;

static bool app_uses_compose_demo(void)
{
    return !k_use_rgb332_framebuffer && !k_use_calibration_card && !k_use_stimulus;
}

static const char *app_render_mode_name(void)
{
    if (k_use_rgb332_framebuffer) {
        return "rgb332_fb";
    }
    if (k_use_calibration_card) {
        return "calibration";
    }
    if (k_use_rgb332_compose) {
        return "rgb332_compose";
    }
    if (k_use_stimulus) {
        return "stimulus";
    }
    return k_enable_color ? "color_bars_ramp" : "luma_bars";
}

/* Demo scene:
 *   layer 0 fused: tile (horizontal scroll per frame)
 *   layer 1 keyed: crt_sprite_layer with bouncing 16x16 sprites
 * Stays on the 1+1 fast path so the prep budget keeps the 0-underrun
 * invariant the hardware just re-validated. */
static crt_compose_viewport_layer_t s_viewport_god; /* reserved, disabled */
static crt_compose_checker_layer_t s_checker;       /* reserved, disabled */
static crt_compose_rect_layer_t s_hud_rect;         /* reserved, disabled */
static uint8_t s_checker_layer_idx = CRT_COMPOSE_LAYER_INVALID;

/* Sprite atlas storage for the PPU facade demo. Sprite rows are patched as
 * spans over the fused tile base, avoiding full-line sprite materialization. */
enum {
#if CONFIG_CRT_COMPOSE_STRESS_DEMO
    APP_DEMO_SPRITE_COUNT = CRT_SPRITE_DEFAULT_PERLINE,
#else
    APP_DEMO_SPRITE_COUNT = 3,
#endif
    APP_SPRITE_ATLAS_W = APP_DEMO_SPRITE_COUNT * 16,
    APP_SPRITE_ATLAS_H = 16,
};

DRAM_ATTR static uint8_t s_sprite_atlas_data[APP_SPRITE_ATLAS_W * APP_SPRITE_ATLAS_H];
static crt_sprite_atlas_t s_sprite_atlas;

static uint8_t s_sprite_ids[APP_DEMO_SPRITE_COUNT];
#if CONFIG_CRT_COMPOSE_STRESS_DEMO
static const uint8_t s_sprite_attrs[APP_DEMO_SPRITE_COUNT] = {
    0U,
    CRT_SPRITE_ATTR_HFLIP,
    CRT_SPRITE_ATTR_VFLIP,
    (uint8_t)(CRT_SPRITE_ATTR_HFLIP | CRT_SPRITE_ATTR_VFLIP),
    (uint8_t)(1U << CRT_SPRITE_ATTR_PALETTE_SHIFT),
    (uint8_t)(CRT_SPRITE_ATTR_HFLIP | (1U << CRT_SPRITE_ATTR_PALETTE_SHIFT)),
    CRT_SPRITE_ATTR_BG_PRIORITY,
    (uint8_t)(CRT_SPRITE_ATTR_BG_PRIORITY | CRT_SPRITE_ATTR_VFLIP |
              (1U << CRT_SPRITE_ATTR_PALETTE_SHIFT)),
};
#else
static const uint8_t s_sprite_attrs[APP_DEMO_SPRITE_COUNT] = {
    0U,
    CRT_SPRITE_ATTR_HFLIP,
    (uint8_t)(CRT_SPRITE_ATTR_VFLIP | (1U << CRT_SPRITE_ATTR_PALETTE_SHIFT)),
};
#endif

static void demo_sprite_atlas_fill(void)
{
    for (size_t s = 0; s < APP_DEMO_SPRITE_COUNT; ++s) {
        const uint8_t color = (uint8_t)(48U + (uint8_t)(s * 24U));
        for (uint8_t y = 0; y < 16U; ++y) {
            uint8_t *row = &s_sprite_atlas_data[(size_t)y * APP_SPRITE_ATLAS_W + (size_t)s * 16U];
            for (uint8_t x = 0; x < 16U; ++x) {
                /* 1px transparent border + filled interior: gives the
                 * sprite a visible silhouette when overlapping the BG. */
                row[x] = (x == 0 || x == 15 || y == 0 || y == 15) ? 0U : color;
            }
        }
    }
}

/* Tile backend storage. Nametable is DRAM-resident (mutable at runtime);
 * pattern_table lives in rodata via tile_demo.h. 32x32 pitch enables the
 * AND-mask wraparound fast path; 32x30 visible matches NTSC/PAL active
 * lines exactly so the compose hot path lands on the 256->768 expansion. */
#define TILE_PITCH_W          32u
#define TILE_PITCH_H          32u
#define TILE_VISIBLE_W        32u
#define TILE_VISIBLE_H        30u
#define TILE_BANK1_LUMA_BOOST 48u
#define TILE_PRIORITY_ROW0    11u
#define TILE_PRIORITY_COL0    12u
#define TILE_PRIORITY_COLS    8u
static uint8_t s_tile_nametable[TILE_PITCH_W * TILE_PITCH_H];
static uint8_t s_tile_attrs[TILE_PITCH_W * TILE_PITCH_H];
static uint8_t s_tile_palette_bank_1[256];
static crt_compose_palette_banks_t s_tile_palette_banks;

static void demo_tile_attrs_fill(void)
{
    memset(s_tile_attrs, 0, sizeof(s_tile_attrs));
    for (uint16_t i = 0; i < 256U; ++i) {
        const uint16_t boosted = (uint16_t)(i + TILE_BANK1_LUMA_BOOST);
        s_tile_palette_bank_1[i] = (uint8_t)((boosted > 255U) ? 255U : boosted);
    }
    memset(&s_tile_palette_banks, 0, sizeof(s_tile_palette_banks));
    s_tile_palette_banks.banks[1] = s_tile_palette_bank_1;

    for (uint16_t row = 0; row < TILE_VISIBLE_H; ++row) {
        for (uint16_t col = 0; col < TILE_VISIBLE_W; ++col) {
            if (((row ^ col) & 7U) == 0U) {
                s_tile_attrs[(size_t)row * TILE_PITCH_W + col] =
                    (uint8_t)(1U << CRT_TILE_ATTR_PALETTE_SHIFT);
            }
            if (row == TILE_PRIORITY_ROW0 && col >= TILE_PRIORITY_COL0 &&
                col < (uint16_t)(TILE_PRIORITY_COL0 + TILE_PRIORITY_COLS)) {
                s_tile_attrs[(size_t)row * TILE_PITCH_W + col] |= CRT_TILE_ATTR_PRIORITY;
            }
        }
    }
}

#define APP_FB_WIDTH    256
#define APP_FB_HEIGHT   240
#define APP_BLANK_LEVEL ((uint16_t)(23U << 8))
#define APP_WHITE_LEVEL ((uint16_t)(0x70U << 8)) /* ~44% DAC — tuned for C270 webcam capture */

/* Frame hook: drives runtime animation. Tile horizontal scroll wraps every
 * visible_w*8 pixels. Sprite mutation remains wired below for future optimized
 * sprite smoke tests; the default hardware path keeps OAM empty. */
IRAM_ATTR static void demo_frame_hook(uint32_t frame, void *user_data)
{
    (void)user_data;
    (void)frame;

    (void)crt_ppu_commit_frame(&s_ppu);
    crt_ppu_set_scroll(&s_ppu, (int)(frame % (TILE_VISIBLE_W * 8U)), 0);

#if CONFIG_CRT_COMPOSE_STRESS_DEMO
    for (size_t i = 0; i < APP_DEMO_SPRITE_COUNT; ++i) {
        if (s_sprite_ids[i] == CRT_SPRITE_INVALID_ID) {
            continue;
        }
        const int16_t x = (int16_t)((frame + (uint32_t)(i * 29U)) % (256U - 16U));
        const int16_t y = 96;
        (void)crt_ppu_stage_sprite_position(&s_ppu, s_sprite_ids[i], x, y);
    }
#else
    /* Sprite world is logical 256x240. Bounce inside
     * [0 .. 256-16] horizontally and [0 .. 240-16] vertically per sprite. */
    static int16_t s_dx[APP_DEMO_SPRITE_COUNT] = {1, -1, 2};
    static int16_t s_dy[APP_DEMO_SPRITE_COUNT] = {1, 2, -1};
    for (size_t i = 0; i < APP_DEMO_SPRITE_COUNT; ++i) {
        if (s_sprite_ids[i] == CRT_SPRITE_INVALID_ID) {
            continue;
        }
        crt_sprite_t spr;
        if (crt_sprite_get(&s_ppu.sprites, s_sprite_ids[i], &spr) != ESP_OK) {
            continue;
        }
        int16_t nx = (int16_t)(spr.x + s_dx[i]);
        int16_t ny = (int16_t)(spr.y + s_dy[i]);
        if (nx <= 0 || nx >= (int16_t)(256 - 16)) {
            s_dx[i] = (int16_t)-s_dx[i];
        }
        if (ny <= 0 || ny >= (int16_t)(240 - 16)) {
            s_dy[i] = (int16_t)-s_dy[i];
        }
        (void)crt_ppu_stage_sprite_position(&s_ppu, s_sprite_ids[i], (int16_t)(spr.x + s_dx[i]),
                                            (int16_t)(spr.y + s_dy[i]));
    }
#endif
}

static void app_fill_rgb332_test_card(crt_fb_surface_t *fb)
{
    if (fb == NULL || fb->buffer == NULL || fb->width == 0 || fb->height == 0) {
        return;
    }

    for (uint16_t y = 0; y < fb->height; ++y) {
        uint8_t *row = crt_fb_row(fb, y);
        uint8_t red = (uint8_t)(((uint32_t)y * 8U / fb->height) << 5);
        for (uint16_t x = 0; x < fb->width; ++x) {
            row[x] = (uint8_t)(red | ((uint32_t)x * 32U / fb->width));
        }
    }
}

/* ── Optional serial upload protocol ─────────────────────────────── */
#if CONFIG_CRT_ENABLE_UART_UPLOAD
static const uint8_t k_upload_magic[4] = {0xFB, 0xDA, 0x00, 0x01};
#define UPLOAD_ACK      0x06
#define UPLOAD_NAK      0x15
#define UPLOAD_UART_NUM UART_NUM_0
#define UPLOAD_RX_BUF   8192

static int s_uart_fd = -1;

static esp_err_t uart_upload_init(void)
{
    if (s_uart_fd >= 0) {
        close(s_uart_fd);
        s_uart_fd = -1;
    }

    uart_vfs_dev_use_driver(UPLOAD_UART_NUM);
    s_uart_fd = open("/dev/uart/0", O_RDWR | O_NONBLOCK);
    if (s_uart_fd < 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "uart upload enabled on /dev/uart/0");
    return ESP_OK;
}

static void uart_upload_check(crt_fb_surface_t *fb)
{
    uint8_t byte;
    const uint8_t *magic = k_upload_magic;

    if (s_uart_fd < 0)
        return;

    /* Non-blocking peek for first magic byte */
    int n = read(s_uart_fd, &byte, 1);
    if (n <= 0)
        return;
    if (byte != magic[0])
        return;

    /* Read remaining magic with blocking reads (short data, should be immediate) */
    uint8_t rest[3];
    size_t got = 0;
    while (got < 3) {
        n = read(s_uart_fd, &rest[got], 3 - got);
        if (n <= 0)
            return;
        got += (size_t)n;
    }
    if (rest[0] != magic[1] || rest[1] != magic[2] || rest[2] != magic[3]) {
        return;
    }

    ESP_LOGI(TAG, "upload: magic received, expecting %u bytes...", (unsigned)fb->buffer_size);

    /* Switch to blocking mode for bulk data transfer */
    int flags = fcntl(s_uart_fd, F_GETFL);
    fcntl(s_uart_fd, F_SETFL, flags & ~O_NONBLOCK);

    /* Receive pixel data */
    size_t received = 0;
    while (received < fb->buffer_size) {
        n = read(s_uart_fd, &fb->buffer[received], fb->buffer_size - received);
        if (n <= 0) {
            ESP_LOGE(TAG, "upload: read error at byte %u/%u (n=%d)", (unsigned)received,
                     (unsigned)fb->buffer_size, n);
            byte = UPLOAD_NAK;
            write(s_uart_fd, &byte, 1);
            fcntl(s_uart_fd, F_SETFL, flags); /* restore non-blocking */
            return;
        }
        received += (size_t)n;
    }

    /* Restore non-blocking mode */
    fcntl(s_uart_fd, F_SETFL, flags);

    byte = UPLOAD_ACK;
    write(s_uart_fd, &byte, 1);
    ESP_LOGI(TAG, "upload: %u bytes received, framebuffer updated!", (unsigned)received);
}

#if CONFIG_CRT_TEST_STANDARD_TOGGLE
static void uart_upload_shutdown(void)
{
    if (s_uart_fd >= 0) {
        close(s_uart_fd);
        s_uart_fd = -1;
    }
}
#endif
#endif

static void app_draw_text_shadow_5x7(crt_fb_surface_t *fb, uint16_t x, uint16_t y, const char *text,
                                     uint8_t fg, uint8_t shadow, uint8_t scale)
{
    if (x < UINT16_MAX && y < UINT16_MAX) {
        crt_fb_draw_text_5x7(fb, (uint16_t)(x + 1U), (uint16_t)(y + 1U), text, shadow, scale);
    }
    crt_fb_draw_text_5x7(fb, x, y, text, fg, scale);
}

static void app_draw_calibration_labels(crt_fb_surface_t *fb,
                                        const crt_timing_standard_info_t *standard_info)
{
    if (fb == NULL || fb->buffer == NULL || standard_info == NULL) {
        return;
    }

    app_draw_text_shadow_5x7(fb, 10, 10, "CAL", 0xff, 0x00, 2);
    app_draw_text_shadow_5x7(fb, 10, 30, standard_info->name, 0xff, 0x00, 1);
    app_draw_text_shadow_5x7(fb, 10, 40, standard_info->experimental ? "EXP" : "VALID", 0xff, 0x00,
                             1);
    app_draw_text_shadow_5x7(fb, 98, 116, "CENTER", 0xff, 0x00, 1);
}

static void app_draw_calibration_runtime_hud(crt_fb_surface_t *fb, const crt_diag_snapshot_t *diag)
{
    if (fb == NULL || fb->buffer == NULL || diag == NULL || !s_app_metadata_valid) {
        return;
    }

    char line[40];
    const uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    const uint16_t blank_lines = (uint16_t)(s_app_timing.total_lines - s_app_timing.active_lines);

    snprintf(line, sizeof(line), "FS %" PRIu32, s_app_timing.sample_rate_hz);
    app_draw_text_shadow_5x7(fb, 10, 52, line, 0xff, 0x00, 1);

    snprintf(line, sizeof(line), "SC %" PRIu32, s_app_standard_info.color_subcarrier_hz);
    app_draw_text_shadow_5x7(fb, 10, 62, line, 0xff, 0x00, 1);

    snprintf(line, sizeof(line), "L %u+%u", (unsigned)s_app_timing.active_lines,
             (unsigned)blank_lines);
    app_draw_text_shadow_5x7(fb, 10, 72, line, 0xff, 0x00, 1);

    snprintf(line, sizeof(line), "T %" PRIu32 "s", uptime_s);
    app_draw_text_shadow_5x7(fb, 174, 178, line, 0xff, 0x00, 1);

    snprintf(line, sizeof(line), "U %" PRIu32, diag->dma_underrun_count);
    app_draw_text_shadow_5x7(fb, 174, 190, line, 0xff, 0x00, 1);

    snprintf(line, sizeof(line), "Q %" PRIu32, diag->ready_queue_min_depth);
    app_draw_text_shadow_5x7(fb, 174, 202, line, 0xff, 0x00, 1);

    snprintf(line, sizeof(line), "P %" PRIu32, diag->prep_cycles_max);
    app_draw_text_shadow_5x7(fb, 174, 214, line, 0xff, 0x00, 1);
}

static void app_draw_calibration_card_with_hud(const crt_diag_snapshot_t *diag)
{
    if (!k_use_calibration_card || s_fb.buffer == NULL || !s_app_metadata_valid) {
        return;
    }

    crt_demo_pattern_fill_rgb332_calibration_card(s_fb.buffer, s_fb.width, s_fb.height, s_fb.width);
    app_draw_calibration_labels(&s_fb, &s_app_standard_info);
    if (diag != NULL) {
        app_draw_calibration_runtime_hud(&s_fb, diag);
    }
}

static esp_err_t app_start_core(crt_video_standard_t video_standard)
{
    crt_timing_profile_t timing = {0};
    crt_timing_standard_info_t standard_info = {0};
    crt_core_config_t config = {
        .video_standard = video_standard,
        .enable_color = k_enable_color || k_use_rgb332_framebuffer || k_use_calibration_card ||
                        k_use_rgb332_compose,
        .demo_pattern_mode = (k_enable_color || k_use_rgb332_framebuffer ||
                              k_use_calibration_card || k_use_rgb332_compose)
                                 ? CRT_DEMO_PATTERN_COLOR_BARS_RAMP
                                 : CRT_DEMO_PATTERN_LUMA_BARS,
        .target_ready_depth = 64,
        .min_ready_depth = 0,
        .prep_task_core = 1,
    };

    esp_err_t timing_err = crt_timing_get_profile(video_standard, &timing);
    if (timing_err != ESP_OK) {
        ESP_LOGE(TAG, "crt_timing_get_profile failed: %s", esp_err_to_name(timing_err));
        return timing_err;
    }
    timing_err = crt_timing_get_standard_info(video_standard, &standard_info);
    if (timing_err != ESP_OK) {
        ESP_LOGE(TAG, "crt_timing_get_standard_info failed: %s", esp_err_to_name(timing_err));
        return timing_err;
    }
    s_app_timing = timing;
    s_app_standard_info = standard_info;
    s_app_metadata_valid = true;
    s_last_diag_underruns = 0;

    esp_err_t err = crt_core_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "crt_core_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = crt_fb_surface_init(&s_fb, APP_FB_WIDTH, APP_FB_HEIGHT, CRT_FB_FORMAT_INDEXED8);
    if (err == ESP_OK) {
        err = crt_fb_surface_alloc(&s_fb);
    }
    if (err == ESP_OK) {
        if (k_use_rgb332_framebuffer) {
            app_fill_rgb332_test_card(&s_fb);
        } else if (k_use_calibration_card) {
            app_draw_calibration_card_with_hud(NULL);
        } else if (k_use_stimulus) {
            crt_fb_palette_init_grayscale(&s_fb, APP_BLANK_LEVEL, APP_WHITE_LEVEL);
        } else {
            crt_fb_palette_init_grayscale(&s_fb, APP_BLANK_LEVEL, APP_WHITE_LEVEL);
            if (s_fb.buffer_size <= sizeof(godzilla_pixels)) {
                memcpy(s_fb.buffer, godzilla_pixels, s_fb.buffer_size);
            }
        }
#if CONFIG_CRT_ENABLE_UART_UPLOAD
        esp_err_t upload_err = uart_upload_init();
        if (upload_err != ESP_OK) {
            ESP_LOGW(TAG, "uart upload disabled: %s", esp_err_to_name(upload_err));
        }
#endif
    } else {
        ESP_LOGE(TAG, "fb alloc failed: %s", esp_err_to_name(err));
        return err;
    }

    if (k_use_rgb332_framebuffer || k_use_calibration_card) {
        crt_register_scanline_hook(crt_fb_rgb332_scanline_hook, &s_fb);
        ESP_LOGI(TAG, "render: %s RGB332 framebuffer %ux%u",
                 k_use_calibration_card ? "calibration" : "direct", s_fb.width, s_fb.height);
    } else if (k_use_stimulus) {
        crt_stimulus_config_t stimulus_config;
        crt_stimulus_default_config(&stimulus_config);
        stimulus_config.height = APP_FB_HEIGHT;
        stimulus_config.pattern = CRT_STIMULUS_PATTERN_CHECKER;
        stimulus_config.cell_w = 8;
        stimulus_config.cell_h = 8;

        esp_err_t stimulus_err = crt_stimulus_init(&s_stimulus, &stimulus_config);
        if (stimulus_err != ESP_OK) {
            ESP_LOGE(TAG, "crt_stimulus_init failed: %s", esp_err_to_name(stimulus_err));
            return stimulus_err;
        }

        stimulus_err = crt_compose_init(&s_stimulus_compose);
        if (stimulus_err == ESP_OK) {
            stimulus_err =
                crt_compose_set_palette(&s_stimulus_compose, s_fb.palette, s_fb.palette_size);
        }
        if (stimulus_err == ESP_OK) {
            stimulus_err = crt_compose_add_layer(&s_stimulus_compose, crt_stimulus_layer_fetch,
                                                 &s_stimulus, CRT_COMPOSE_NO_TRANSPARENCY);
        }
        if (stimulus_err != ESP_OK) {
            ESP_LOGE(TAG, "stimulus compose setup failed: %s", esp_err_to_name(stimulus_err));
            return stimulus_err;
        }

        crt_register_scanline_hook(crt_compose_scanline_hook, &s_stimulus_compose);
        ESP_LOGI(TAG, "render: measurement stimulus checker (%ux%u, cell=%ux%u)", APP_FB_WIDTH,
                 APP_FB_HEIGHT, stimulus_config.cell_w, stimulus_config.cell_h);
    } else {
        /* ── Tile layer as fused base + keyed overlay on top ──────────── */

        /* Tile layer: 32x32 pitch (PoT), 32x30 visible. Pattern from
         * rodata (tile_demo.h); nametable filled in DRAM. */
        tile_demo_fill_nametable(s_tile_nametable, TILE_PITCH_W);
        demo_tile_attrs_fill();
        demo_sprite_atlas_fill();
        crt_sprite_atlas_init(&s_sprite_atlas, s_sprite_atlas_data, APP_SPRITE_ATLAS_W,
                              APP_SPRITE_ATLAS_H, APP_SPRITE_ATLAS_W);

        crt_ppu_config_t ppu_config = {
            .visible_w_tiles = TILE_VISIBLE_W,
            .visible_h_tiles = TILE_VISIBLE_H,
            .pitch_w_tiles = TILE_PITCH_W,
            .pitch_h_tiles = TILE_PITCH_H,
            .pattern_table = tile_demo_patterns,
            .pattern_count = TILE_DEMO_COUNT,
            .nametable = s_tile_nametable,
            .attributes = s_tile_attrs,
            .sprite_atlas = &s_sprite_atlas,
            .sprite_transparent_idx = 0,
            .max_sprites_per_line = CRT_SPRITE_DEFAULT_PERLINE,
            .palette = s_fb.palette,
            .palette_size = s_fb.palette_size,
            .palette_banks = &s_tile_palette_banks,
        };
        esp_err_t ppu_err = crt_ppu_init(&s_ppu, &ppu_config);
        if (ppu_err != ESP_OK) {
            ESP_LOGE(TAG, "crt_ppu_init failed: %s", esp_err_to_name(ppu_err));
            return ppu_err;
        }

        for (size_t i = 0; i < APP_DEMO_SPRITE_COUNT; ++i) {
            s_sprite_ids[i] = CRT_SPRITE_INVALID_ID;
        }
#if CONFIG_CRT_COMPOSE_STRESS_DEMO
        for (size_t i = 0; i < APP_DEMO_SPRITE_COUNT; ++i) {
            ppu_err =
                crt_ppu_add_sprite(&s_ppu, (uint16_t)(i * 2U), 0, CRT_SPRITE_SIZE_16X16,
                                   (int16_t)(i * 29U), 96, s_sprite_attrs[i], &s_sprite_ids[i]);
            if (ppu_err != ESP_OK) {
                ESP_LOGE(TAG, "crt_ppu_add_sprite[%u] failed: %s", (unsigned)i,
                         esp_err_to_name(ppu_err));
                return ppu_err;
            }
        }
#else
        static const int16_t k_sprite_x[APP_DEMO_SPRITE_COUNT] = {24, 112, 184};
        static const int16_t k_sprite_y[APP_DEMO_SPRITE_COUNT] = {32, 96, 152};
        for (size_t i = 0; i < APP_DEMO_SPRITE_COUNT; ++i) {
            ppu_err = crt_ppu_add_sprite(&s_ppu, (uint16_t)(i * 2U), 0, CRT_SPRITE_SIZE_16X16,
                                         k_sprite_x[i], k_sprite_y[i], s_sprite_attrs[i],
                                         &s_sprite_ids[i]);
            if (ppu_err != ESP_OK) {
                ESP_LOGE(TAG, "crt_ppu_add_sprite[%u] failed: %s", (unsigned)i,
                         esp_err_to_name(ppu_err));
                return ppu_err;
            }
        }
#endif

        /* Reserved primitives (kept declared for the next iteration). */
        (void)s_viewport_god;
        (void)s_checker;
        (void)s_hud_rect;
        s_checker_layer_idx = CRT_COMPOSE_LAYER_INVALID;

        crt_register_scanline_hook(k_use_rgb332_compose ? crt_ppu_scanline_hook_rgb332_256
                                                        : crt_ppu_scanline_hook,
                                   &s_ppu);
        crt_register_frame_hook(demo_frame_hook, NULL);
        ESP_LOGI(TAG, "compose: %s tile %ux%u (h-scroll) + sprite layer (%u active, max/line=%u%s)",
                 k_use_rgb332_compose ? "rgb332" : "palette", TILE_VISIBLE_W, TILE_VISIBLE_H,
                 (unsigned)APP_DEMO_SPRITE_COUNT, (unsigned)CRT_SPRITE_DEFAULT_PERLINE,
                 CONFIG_CRT_COMPOSE_STRESS_DEMO ? ", stress" : "");
    }

    err = crt_core_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "crt_core_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "ESP32 CRT signal core started: standard=%s color=%s pattern=%s sample=%" PRIu32
             " subcarrier=%" PRIu32 " chroma=%s status=%s",
             standard_info.name, config.enable_color ? "on" : "off", app_render_mode_name(),
             timing.sample_rate_hz, standard_info.color_subcarrier_hz,
             standard_info.chroma_phase_alternates ? "alternate" : "fixed",
             standard_info.experimental ? "experimental" : "validated");

    return ESP_OK;
}

static void app_log_diag_snapshot(void)
{
    crt_diag_snapshot_t diag;
    crt_core_get_diag_snapshot(&diag);
    app_draw_calibration_card_with_hud(&diag);

    const uint32_t underrun_delta = diag.dma_underrun_count - s_last_diag_underruns;
    s_last_diag_underruns = diag.dma_underrun_count;
    const uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);

    if (s_app_metadata_valid) {
        ESP_LOGI(TAG,
                 "runtime_meta: t=%" PRIu32 "s standard=%s render=%s active=%u total=%u "
                 "field=%u.%03uHz sample=%" PRIu32 " subcarrier=%" PRIu32
                 " chroma=%s status=%s underruns=%" PRIu32 " delta=%" PRIu32 " queue_min=%" PRIu32
                 " prep_max=%" PRIu32 " cycles",
                 uptime_s, s_app_standard_info.name, app_render_mode_name(),
                 (unsigned)s_app_timing.active_lines, (unsigned)s_app_timing.total_lines,
                 (unsigned)(s_app_standard_info.field_rate_millihz / 1000U),
                 (unsigned)(s_app_standard_info.field_rate_millihz % 1000U),
                 s_app_timing.sample_rate_hz, s_app_standard_info.color_subcarrier_hz,
                 s_app_standard_info.chroma_phase_alternates ? "alternate" : "fixed",
                 s_app_standard_info.experimental ? "experimental" : "validated",
                 diag.dma_underrun_count, underrun_delta, diag.ready_queue_min_depth,
                 diag.prep_cycles_max);
    } else {
        ESP_LOGI(TAG,
                 "runtime_meta: t=%" PRIu32 "s underruns=%" PRIu32 " delta=%" PRIu32
                 " queue_min=%" PRIu32 " prep_max=%" PRIu32 " cycles",
                 uptime_s, diag.dma_underrun_count, underrun_delta, diag.ready_queue_min_depth,
                 diag.prep_cycles_max);
    }
    if (app_uses_compose_demo()) {
        const uint32_t sprite_overflow = crt_ppu_get_sprite_overflow_count(&s_ppu);
        const uint8_t sprite_peak = crt_ppu_get_sprite_max_line_rendered(&s_ppu);
        const uint8_t ppu_pending = crt_ppu_get_pending_update_count(&s_ppu);
        const crt_compose_stats_t compose_stats = crt_ppu_get_compose_stats(&s_ppu);
        ESP_LOGI(TAG,
                 "compose_budget: mode=%s attrs=tile banks=tile scroll=h sprites=%u max/line=%u "
                 "sprite_attrs=flip+bank priority=%s active=%ux%u bank1=+%u sprite_peak=%u/%u "
                 "sprite_overflow=%" PRIu32 " ppu_pending=%u/%u compose_fused=%" PRIu32
                 " compose_materialized=%" PRIu32 " compose_max_layers=%u underruns=%" PRIu32
                 " queue_min=%" PRIu32 " prep_max=%" PRIu32 " cycles",
                 k_use_rgb332_compose ? "rgb332" : "palette", (unsigned)APP_DEMO_SPRITE_COUNT,
                 (unsigned)CRT_SPRITE_DEFAULT_PERLINE,
                 CONFIG_CRT_COMPOSE_STRESS_DEMO ? "tile+sprite" : "tile",
                 (unsigned)(TILE_VISIBLE_W * CRT_TILE_PX_W),
                 (unsigned)(TILE_VISIBLE_H * CRT_TILE_PX_H), (unsigned)TILE_BANK1_LUMA_BOOST,
                 (unsigned)sprite_peak, (unsigned)CRT_SPRITE_DEFAULT_PERLINE, sprite_overflow,
                 (unsigned)ppu_pending, (unsigned)CRT_PPU_MAX_PENDING_UPDATES,
                 compose_stats.fused_lines, compose_stats.materialized_lines,
                 (unsigned)compose_stats.max_layers_fetched, diag.dma_underrun_count,
                 diag.ready_queue_min_depth, diag.prep_cycles_max);
        crt_ppu_reset_compose_stats(&s_ppu);
        crt_ppu_reset_sprite_stats(&s_ppu);
    }
}

#if !CONFIG_CRT_TEST_STANDARD_TOGGLE
static void app_run_diag_loop(void)
{
    while (true) {
#if CONFIG_CRT_ENABLE_UART_UPLOAD
        uart_upload_check(&s_fb);
        vTaskDelay(pdMS_TO_TICKS(10));
#else
        vTaskDelay(pdMS_TO_TICKS(APP_DIAG_INTERVAL_MS));
#endif

        static uint32_t ticks_since_diag;
        ticks_since_diag +=
#if CONFIG_CRT_ENABLE_UART_UPLOAD
            10U;
#else
            APP_DIAG_INTERVAL_MS;
#endif
        if (ticks_since_diag >= APP_DIAG_INTERVAL_MS) {
            ticks_since_diag = 0;
            app_log_diag_snapshot();
        }
    }
}
#endif

#if CONFIG_CRT_TEST_STANDARD_TOGGLE
static void app_cleanup_core(void)
{

#if CONFIG_CRT_ENABLE_UART_UPLOAD
    uart_upload_shutdown();
#endif
    crt_register_scanline_hook(NULL, NULL);
    crt_register_frame_hook(NULL, NULL);
    crt_compose_clear_layers(&s_ppu.compose);
    crt_compose_clear_layers(&s_stimulus_compose);
    crt_fb_surface_deinit(&s_fb);
}

static void app_run_standard_toggle_loop(crt_video_standard_t video_standard)
{
    while (true) {
        esp_err_t err;

#if CONFIG_CRT_ENABLE_UART_UPLOAD
        uart_upload_check(&s_fb);
#endif
        vTaskDelay(pdMS_TO_TICKS((uint32_t)CONFIG_CRT_TEST_STANDARD_TOGGLE_INTERVAL_S * 1000U));
        video_standard = (video_standard == CRT_VIDEO_STANDARD_NTSC) ? CRT_VIDEO_STANDARD_PAL
                                                                     : CRT_VIDEO_STANDARD_NTSC;

        ESP_LOGI(TAG, "test toggle: switching standard to %s (interval=%" PRIu32 "s)",
                 crt_timing_get_standard_name(video_standard),
                 (uint32_t)CONFIG_CRT_TEST_STANDARD_TOGGLE_INTERVAL_S);

        err = crt_core_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "crt_core_stop failed during toggle: %s", esp_err_to_name(err));
            break;
        }

        app_cleanup_core();

        err = crt_core_deinit();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "crt_core_deinit failed during toggle: %s", esp_err_to_name(err));
            break;
        }
        if (app_start_core(video_standard) != ESP_OK) {
            break;
        }
    }
}
#endif

void app_main(void)
{
    crt_video_standard_t video_standard =
#if CONFIG_CRT_VIDEO_STANDARD_PAL
        CRT_VIDEO_STANDARD_PAL;
#elif CONFIG_CRT_VIDEO_STANDARD_PAL_M
        CRT_VIDEO_STANDARD_PAL_M;
#elif CONFIG_CRT_VIDEO_STANDARD_PAL_N
        CRT_VIDEO_STANDARD_PAL_N;
#else
        CRT_VIDEO_STANDARD_NTSC;
#endif

    if (app_start_core(video_standard) != ESP_OK) {
        return;
    }

#if CONFIG_CRT_TEST_STANDARD_TOGGLE
    app_run_standard_toggle_loop(video_standard);
#else
    app_run_diag_loop();
#endif
}
