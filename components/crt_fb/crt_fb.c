#include "crt_fb.h"

#include "crt_composite_palette.h"

#include "esp_attr.h"
#include "esp_check.h"

#include <stdlib.h>
#include <string.h>

#define CRT_FB_PALETTE_SIZE_INDEXED8 256

/* ── Lifecycle ────────────────────────────────────────────────────── */

esp_err_t crt_fb_surface_init(crt_fb_surface_t *surface, uint16_t width, uint16_t height,
                              crt_fb_format_t format)
{
    ESP_RETURN_ON_FALSE(surface != NULL, ESP_ERR_INVALID_ARG, "crt_fb", "surface is null");
    ESP_RETURN_ON_FALSE(width > 0 && height > 0, ESP_ERR_INVALID_ARG, "crt_fb", "zero dimension");

    *surface = (crt_fb_surface_t){
        .width = width,
        .height = height,
        .format = format,
        .buffer = NULL,
        .buffer_size = 0,
        .palette = NULL,
        .palette_size = 0,
    };
    return ESP_OK;
}

esp_err_t crt_fb_surface_alloc(crt_fb_surface_t *surface)
{
    size_t buf_size;
    uint16_t palette_entries;

    ESP_RETURN_ON_FALSE(surface != NULL, ESP_ERR_INVALID_ARG, "crt_fb", "surface is null");
    ESP_RETURN_ON_FALSE(surface->buffer == NULL, ESP_ERR_INVALID_STATE, "crt_fb",
                        "already allocated");

    switch (surface->format) {
    case CRT_FB_FORMAT_INDEXED8:
        buf_size = (size_t)surface->width * surface->height;
        palette_entries = CRT_FB_PALETTE_SIZE_INDEXED8;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    surface->buffer = calloc(1, buf_size);
    ESP_RETURN_ON_FALSE(surface->buffer != NULL, ESP_ERR_NO_MEM, "crt_fb",
                        "buffer alloc failed (%u bytes)", (unsigned)buf_size);
    surface->buffer_size = buf_size;

    surface->palette = calloc(palette_entries, sizeof(uint16_t));
    if (surface->palette == NULL) {
        free(surface->buffer);
        surface->buffer = NULL;
        surface->buffer_size = 0;
        return ESP_ERR_NO_MEM;
    }
    surface->palette_size = palette_entries;

    return ESP_OK;
}

esp_err_t crt_fb_surface_free(crt_fb_surface_t *surface)
{
    ESP_RETURN_ON_FALSE(surface != NULL, ESP_ERR_INVALID_ARG, "crt_fb", "surface is null");

    free(surface->buffer);
    surface->buffer = NULL;
    surface->buffer_size = 0;
    free(surface->palette);
    surface->palette = NULL;
    surface->palette_size = 0;

    return ESP_OK;
}

esp_err_t crt_fb_surface_deinit(crt_fb_surface_t *surface)
{
    ESP_RETURN_ON_FALSE(surface != NULL, ESP_ERR_INVALID_ARG, "crt_fb", "surface is null");

    if (surface->buffer != NULL) {
        crt_fb_surface_free(surface);
    }
    memset(surface, 0, sizeof(*surface));
    return ESP_OK;
}

/* ── Pixel access ─────────────────────────────────────────────────── */

uint8_t *crt_fb_row(const crt_fb_surface_t *surface, uint16_t y)
{
    if (surface == NULL || surface->buffer == NULL || y >= surface->height) {
        return NULL;
    }
    return &surface->buffer[(size_t)y * surface->width];
}

void crt_fb_put(crt_fb_surface_t *surface, uint16_t x, uint16_t y, uint8_t value)
{
    if (surface == NULL || surface->buffer == NULL) {
        return;
    }
    if (x < surface->width && y < surface->height) {
        surface->buffer[(size_t)y * surface->width + x] = value;
    }
}

uint8_t crt_fb_get(const crt_fb_surface_t *surface, uint16_t x, uint16_t y)
{
    if (surface == NULL || surface->buffer == NULL) {
        return 0;
    }
    if (x < surface->width && y < surface->height) {
        return surface->buffer[(size_t)y * surface->width + x];
    }
    return 0;
}

void crt_fb_clear(crt_fb_surface_t *surface, uint8_t value)
{
    if (surface != NULL && surface->buffer != NULL) {
        memset(surface->buffer, value, surface->buffer_size);
    }
}

void crt_fb_hline(crt_fb_surface_t *surface, uint16_t x, uint16_t y, uint16_t width, uint8_t value)
{
    if (surface == NULL || surface->buffer == NULL || width == 0 || y >= surface->height ||
        x >= surface->width) {
        return;
    }

    uint16_t clipped_width = width;
    if ((uint32_t)x + clipped_width > surface->width) {
        clipped_width = (uint16_t)(surface->width - x);
    }
    memset(&surface->buffer[(size_t)y * surface->width + x], value, clipped_width);
}

void crt_fb_vline(crt_fb_surface_t *surface, uint16_t x, uint16_t y, uint16_t height, uint8_t value)
{
    if (surface == NULL || surface->buffer == NULL || height == 0 || x >= surface->width ||
        y >= surface->height) {
        return;
    }

    uint16_t clipped_height = height;
    if ((uint32_t)y + clipped_height > surface->height) {
        clipped_height = (uint16_t)(surface->height - y);
    }
    for (uint16_t row = 0; row < clipped_height; ++row) {
        surface->buffer[(size_t)(y + row) * surface->width + x] = value;
    }
}

void crt_fb_fill_rect(crt_fb_surface_t *surface, uint16_t x, uint16_t y, uint16_t width,
                      uint16_t height, uint8_t value)
{
    if (surface == NULL || surface->buffer == NULL || width == 0 || height == 0 ||
        x >= surface->width || y >= surface->height) {
        return;
    }

    uint16_t clipped_width = width;
    uint16_t clipped_height = height;
    if ((uint32_t)x + clipped_width > surface->width) {
        clipped_width = (uint16_t)(surface->width - x);
    }
    if ((uint32_t)y + clipped_height > surface->height) {
        clipped_height = (uint16_t)(surface->height - y);
    }
    for (uint16_t row = 0; row < clipped_height; ++row) {
        memset(&surface->buffer[(size_t)(y + row) * surface->width + x], value, clipped_width);
    }
}

void crt_fb_rect(crt_fb_surface_t *surface, uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                 uint8_t value)
{
    if (surface == NULL || surface->buffer == NULL || width == 0 || height == 0 ||
        x >= surface->width || y >= surface->height) {
        return;
    }

    uint16_t clipped_width = width;
    uint16_t clipped_height = height;
    if ((uint32_t)x + clipped_width > surface->width) {
        clipped_width = (uint16_t)(surface->width - x);
    }
    if ((uint32_t)y + clipped_height > surface->height) {
        clipped_height = (uint16_t)(surface->height - y);
    }

    crt_fb_hline(surface, x, y, clipped_width, value);
    if (clipped_height > 1) {
        crt_fb_hline(surface, x, (uint16_t)(y + clipped_height - 1U), clipped_width, value);
    }
    if (clipped_height > 2) {
        crt_fb_vline(surface, x, (uint16_t)(y + 1U), (uint16_t)(clipped_height - 2U), value);
        if (clipped_width > 1) {
            crt_fb_vline(surface, (uint16_t)(x + clipped_width - 1U), (uint16_t)(y + 1U),
                         (uint16_t)(clipped_height - 2U), value);
        }
    }
}

static const uint8_t k_font_5x7_digits[10][7] = {
    {0x0EU, 0x11U, 0x13U, 0x15U, 0x19U, 0x11U, 0x0EU},
    {0x04U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU},
    {0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x08U, 0x1FU},
    {0x1EU, 0x01U, 0x01U, 0x0EU, 0x01U, 0x01U, 0x1EU},
    {0x02U, 0x06U, 0x0AU, 0x12U, 0x1FU, 0x02U, 0x02U},
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x01U, 0x01U, 0x1EU},
    {0x0EU, 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x0EU},
    {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x08U, 0x08U},
    {0x0EU, 0x11U, 0x11U, 0x0EU, 0x11U, 0x11U, 0x0EU},
    {0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x01U, 0x0EU},
};

static const uint8_t k_font_5x7_letters[26][7] = {
    {0x0EU, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U},
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x11U, 0x11U, 0x1EU},
    {0x0EU, 0x11U, 0x10U, 0x10U, 0x10U, 0x11U, 0x0EU},
    {0x1EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x1EU},
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x1FU},
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x10U},
    {0x0EU, 0x11U, 0x10U, 0x17U, 0x11U, 0x11U, 0x0FU},
    {0x11U, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U},
    {0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x1FU},
    {0x01U, 0x01U, 0x01U, 0x01U, 0x11U, 0x11U, 0x0EU},
    {0x11U, 0x12U, 0x14U, 0x18U, 0x14U, 0x12U, 0x11U},
    {0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x1FU},
    {0x11U, 0x1BU, 0x15U, 0x15U, 0x11U, 0x11U, 0x11U},
    {0x11U, 0x19U, 0x15U, 0x13U, 0x11U, 0x11U, 0x11U},
    {0x0EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU},
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x10U, 0x10U, 0x10U},
    {0x0EU, 0x11U, 0x11U, 0x11U, 0x15U, 0x12U, 0x0DU},
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x14U, 0x12U, 0x11U},
    {0x0FU, 0x10U, 0x10U, 0x0EU, 0x01U, 0x01U, 0x1EU},
    {0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U},
    {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU},
    {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0AU, 0x04U},
    {0x11U, 0x11U, 0x11U, 0x15U, 0x15U, 0x1BU, 0x11U},
    {0x11U, 0x11U, 0x0AU, 0x04U, 0x0AU, 0x11U, 0x11U},
    {0x11U, 0x11U, 0x0AU, 0x04U, 0x04U, 0x04U, 0x04U},
    {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x1FU},
};

static const uint8_t *crt_fb_font_5x7_rows(char ch)
{
    static const uint8_t k_dash[7] = {0x00U, 0x00U, 0x00U, 0x1FU, 0x00U, 0x00U, 0x00U};
    static const uint8_t k_dot[7] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x0CU, 0x0CU};
    static const uint8_t k_colon[7] = {0x00U, 0x0CU, 0x0CU, 0x00U, 0x0CU, 0x0CU, 0x00U};
    static const uint8_t k_slash[7] = {0x01U, 0x02U, 0x02U, 0x04U, 0x08U, 0x08U, 0x10U};

    if (ch >= '0' && ch <= '9') {
        return k_font_5x7_digits[ch - '0'];
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - ('a' - 'A'));
    }
    if (ch >= 'A' && ch <= 'Z') {
        return k_font_5x7_letters[ch - 'A'];
    }
    switch (ch) {
    case '-':
        return k_dash;
    case '.':
        return k_dot;
    case ':':
        return k_colon;
    case '/':
        return k_slash;
    default:
        return NULL;
    }
}

void crt_fb_draw_char_5x7(crt_fb_surface_t *surface, uint16_t x, uint16_t y, char ch, uint8_t value,
                          uint8_t scale)
{
    const uint8_t *rows = crt_fb_font_5x7_rows(ch);

    if (surface == NULL || surface->buffer == NULL || rows == NULL || scale == 0) {
        return;
    }

    for (uint8_t row = 0; row < 7U; ++row) {
        for (uint8_t col = 0; col < 5U; ++col) {
            if ((rows[row] & (uint8_t)(1U << (4U - col))) == 0U) {
                continue;
            }
            const uint32_t px = (uint32_t)x + (uint32_t)col * scale;
            const uint32_t py = (uint32_t)y + (uint32_t)row * scale;
            if (px > UINT16_MAX || py > UINT16_MAX) {
                continue;
            }
            if (scale == 1U) {
                crt_fb_put(surface, (uint16_t)px, (uint16_t)py, value);
            } else {
                crt_fb_fill_rect(surface, (uint16_t)px, (uint16_t)py, scale, scale, value);
            }
        }
    }
}

void crt_fb_draw_text_5x7(crt_fb_surface_t *surface, uint16_t x, uint16_t y, const char *text,
                          uint8_t value, uint8_t scale)
{
    if (surface == NULL || text == NULL || scale == 0) {
        return;
    }

    uint32_t cursor_x = x;
    const uint16_t advance = (uint16_t)(6U * scale);
    while (*text != '\0') {
        if (*text != ' ' && cursor_x <= UINT16_MAX) {
            crt_fb_draw_char_5x7(surface, (uint16_t)cursor_x, y, *text, value, scale);
        }
        cursor_x += advance;
        ++text;
    }
}

/* ── Palette ──────────────────────────────────────────────────────── */

void crt_fb_palette_set(crt_fb_surface_t *surface, uint8_t index, uint16_t dac_level)
{
    if (surface != NULL && surface->palette != NULL && index < surface->palette_size) {
        surface->palette[index] = dac_level;
    }
}

void crt_fb_palette_init_grayscale(crt_fb_surface_t *surface, uint16_t blank_level,
                                   uint16_t white_level)
{
    if (surface == NULL || surface->palette == NULL || surface->palette_size < 2) {
        return;
    }
    for (uint16_t i = 0; i < surface->palette_size; ++i) {
        surface->palette[i] = (uint16_t)(blank_level + (uint32_t)i * (white_level - blank_level) /
                                                           (surface->palette_size - 1));
    }
}

/* ── Scanline hook ────────────────────────────────────────────────── */

IRAM_ATTR void crt_fb_scanline_hook(const crt_scanline_t *scanline, uint16_t *active_buf,
                                    uint16_t active_width, void *user_data)
{
    const crt_fb_surface_t *surface = (const crt_fb_surface_t *)user_data;

    if (scanline == NULL || active_buf == NULL || active_width == 0 || surface == NULL ||
        !CRT_SCANLINE_HAS_LOGICAL(scanline) || scanline->logical_line >= surface->height ||
        surface->buffer == NULL || surface->palette == NULL) {
        return;
    }

    const uint8_t *row = &surface->buffer[(size_t)scanline->logical_line * surface->width];
    const uint16_t *pal = surface->palette;

    /* Fixed-point 16.16 stepping with I2S word-swap baked in.
     * Writes pairs of pixels in swapped order (index ^ 1) to avoid
     * the separate word-swap pass. ~3 cycles/pixel on Xtensa LX6. */
    uint32_t step = ((uint32_t)surface->width << 16) / active_width;
    uint32_t acc = 0;
    uint16_t i = 0;
    const uint16_t even_width = active_width & ~1U;

    for (; i < even_width; i += 2) {
        uint16_t p0 = pal[row[acc >> 16]];
        acc += step;
        uint16_t p1 = pal[row[acc >> 16]];
        acc += step;
        /* I2S expects swapped 16-bit words within each 32-bit DMA word */
        active_buf[i] = p1;
        active_buf[i + 1] = p0;
    }
    if (i < active_width) {
        active_buf[i] = pal[row[acc >> 16]];
    }
}

IRAM_ATTR void crt_fb_rgb332_scanline_hook(const crt_scanline_t *scanline, uint16_t *active_buf,
                                           uint16_t active_width, void *user_data)
{
    const crt_fb_surface_t *surface = (const crt_fb_surface_t *)user_data;
    uint8_t rgb332_row[CRT_COMPOSITE_RGB332_WIDTH];

    if (scanline == NULL || active_buf == NULL ||
        active_width != CRT_COMPOSITE_RGB332_ACTIVE_WIDTH || surface == NULL ||
        scanline->timing == NULL || !CRT_SCANLINE_HAS_LOGICAL(scanline) ||
        scanline->logical_line >= surface->height || surface->buffer == NULL ||
        surface->width == 0) {
        return;
    }

    const uint8_t *row = &surface->buffer[(size_t)scanline->logical_line * surface->width];
    if (surface->width == CRT_COMPOSITE_RGB332_WIDTH) {
        crt_composite_rgb332_render_256_to_768(scanline->timing->standard, scanline->physical_line,
                                               row, active_buf);
        return;
    }

    uint32_t step = ((uint32_t)surface->width << 16) / CRT_COMPOSITE_RGB332_WIDTH;
    uint32_t acc = 0;
    for (uint16_t x = 0; x < CRT_COMPOSITE_RGB332_WIDTH; ++x) {
        rgb332_row[x] = row[acc >> 16];
        acc += step;
    }
    crt_composite_rgb332_render_256_to_768(scanline->timing->standard, scanline->physical_line,
                                           rgb332_row, active_buf);
}

/* ── Compose layer adapter ────────────────────────────────────────── */

IRAM_ATTR bool crt_fb_layer_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                  uint16_t width)
{
    const crt_fb_surface_t *surface = (const crt_fb_surface_t *)ctx;

    if (idx_out == NULL || width == 0) {
        return false;
    }
    if (surface == NULL || surface->buffer == NULL || logical_line >= surface->height ||
        surface->width == 0) {
        memset(idx_out, 0, width);
        return true;
    }

    const uint8_t *row = &surface->buffer[(size_t)logical_line * surface->width];
    if (width == surface->width) {
        memcpy(idx_out, row, width);
        return true;
    }
    uint32_t step = ((uint32_t)surface->width << 16) / width;
    uint32_t acc = 0;
    for (uint16_t x = 0; x < width; ++x) {
        idx_out[x] = row[acc >> 16];
        acc += step;
    }
    return true;
}
