#include "crt_demo_pattern.h"

#define CRT_DEMO_RAMP_HEIGHT_LINES 32U
#define CRT_DEMO_LUMA_BLACK        6400U
#define CRT_DEMO_LUMA_WHITE        18176U

static const uint16_t k_luma_bar_levels[8] = {
    18176U, /* white  100% */
    16880U, /* yellow  89% */
    14643U, /* cyan    70% */
    13348U, /* green   59% */
    11228U, /* magenta 41% */
    9933U,  /* red     30% */
    7695U,  /* blue    11% */
    6400U,  /* black    0% */
};

#define CRT_DEMO_COLOR_BAR_COUNT    8U
#define CRT_DEMO_SAMPLES_PER_QUAD   12U
#define CRT_DEMO_LOGICAL_QUAD_WIDTH 4U

static const uint32_t k_demo_ntsc_bar_patterns[CRT_DEMO_COLOR_BAR_COUNT] = {
    0x49494949U, /* white */
    0x454c413aU, /* yellow */
    0x44323042U, /* cyan */
    0x40352834U, /* green */
    0x202b382cU, /* magenta */
    0x1c2e301eU, /* red */
    0x1b141f26U, /* blue */
    0x18181818U, /* black */
};

static const uint32_t k_demo_pal_bar_patterns_even[CRT_DEMO_COLOR_BAR_COUNT] = {
    0x49494949U, /* white */
    0x47514035U, /* yellow */
    0x27364e3fU, /* cyan */
    0x243e452cU, /* green */
    0x3d231c35U, /* magenta */
    0x3a2b1322U, /* red */
    0x1a10212cU, /* blue */
    0x18181818U, /* black */
};

static const uint32_t k_demo_pal_bar_patterns_odd[CRT_DEMO_COLOR_BAR_COUNT] = {
    0x49494949U, /* white */
    0x40514735U, /* yellow */
    0x4e36273fU, /* cyan */
    0x453e242cU, /* green */
    0x1c233d35U, /* magenta */
    0x132b3a22U, /* red */
    0x21101a2cU, /* blue */
    0x18181818U, /* black */
};

static uint16_t crt_demo_pattern_sample_p0(uint32_t packed)
{
    return (uint16_t)((packed << 8) & 0xFF00U);
}

static uint16_t crt_demo_pattern_sample_p1(uint32_t packed)
{
    return (uint16_t)(packed & 0xFF00U);
}

static uint16_t crt_demo_pattern_sample_p2(uint32_t packed)
{
    return (uint16_t)((packed >> 8) & 0xFF00U);
}

static uint16_t crt_demo_pattern_sample_p3(uint32_t packed)
{
    return (uint16_t)((packed >> 16) & 0xFF00U);
}

static void crt_demo_pattern_encode_legacy_quad(const uint32_t *packed_colors, uint16_t *dst)
{
    dst[0] = crt_demo_pattern_sample_p0(packed_colors[0]);
    dst[1] = crt_demo_pattern_sample_p1(packed_colors[0]);
    dst[2] = crt_demo_pattern_sample_p2(packed_colors[0]);
    dst[3] = crt_demo_pattern_sample_p3(packed_colors[1]);
    dst[4] = crt_demo_pattern_sample_p0(packed_colors[1]);
    dst[5] = crt_demo_pattern_sample_p1(packed_colors[1]);
    dst[6] = crt_demo_pattern_sample_p2(packed_colors[2]);
    dst[7] = crt_demo_pattern_sample_p3(packed_colors[2]);
    dst[8] = crt_demo_pattern_sample_p0(packed_colors[2]);
    dst[9] = crt_demo_pattern_sample_p1(packed_colors[3]);
    dst[10] = crt_demo_pattern_sample_p2(packed_colors[3]);
    dst[11] = crt_demo_pattern_sample_p3(packed_colors[3]);
}

static void crt_demo_pattern_render_color_bars(const crt_demo_pattern_runtime_t *runtime,
                                               const crt_demo_pattern_render_context_t *ctx,
                                               uint16_t *samples, size_t sample_count)
{
    const uint32_t *palette = k_demo_ntsc_bar_patterns;
    uint32_t packed_colors[CRT_DEMO_LOGICAL_QUAD_WIDTH];

    if (sample_count < (size_t)CRT_DEMO_PATTERN_LOGICAL_WIDTH * 3U) {
        return;
    }

    if (ctx->video_standard == CRT_VIDEO_STANDARD_PAL ||
        ctx->video_standard == CRT_VIDEO_STANDARD_PAL_M ||
        ctx->video_standard == CRT_VIDEO_STANDARD_PAL_N) {
        palette = ((ctx->line_index & 0x1U) == 0U) ? k_demo_pal_bar_patterns_even
                                                   : k_demo_pal_bar_patterns_odd;
    }

    for (size_t pixel_index = 0, sample_index = 0; pixel_index < CRT_DEMO_PATTERN_LOGICAL_WIDTH;
         pixel_index += CRT_DEMO_LOGICAL_QUAD_WIDTH, sample_index += CRT_DEMO_SAMPLES_PER_QUAD) {
        for (size_t i = 0; i < CRT_DEMO_LOGICAL_QUAD_WIDTH; ++i) {
            packed_colors[i] = palette[runtime->color_bars_row[pixel_index + i] & 0x7U];
        }
        crt_demo_pattern_encode_legacy_quad(packed_colors, &samples[sample_index]);
    }
}

static void crt_demo_pattern_render_luma_bars(const crt_demo_pattern_runtime_t *runtime,
                                              uint16_t *samples, size_t sample_count)
{
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t pixel_index = (i * CRT_DEMO_PATTERN_LOGICAL_WIDTH) / sample_count;
        const uint8_t bar_index =
            runtime->color_bars_row[pixel_index < CRT_DEMO_PATTERN_LOGICAL_WIDTH
                                        ? pixel_index
                                        : (CRT_DEMO_PATTERN_LOGICAL_WIDTH - 1U)];
        samples[i] = k_luma_bar_levels[bar_index & 0x7U];
    }
}

static uint8_t crt_demo_pixel_for_sample(size_t sample_index, size_t sample_count,
                                         const uint8_t *row)
{
    const size_t pixel_index = (sample_index * CRT_DEMO_PATTERN_LOGICAL_WIDTH) / sample_count;
    return row[pixel_index < CRT_DEMO_PATTERN_LOGICAL_WIDTH
                   ? pixel_index
                   : (CRT_DEMO_PATTERN_LOGICAL_WIDTH - 1U)];
}

static bool crt_demo_marker_pixel_for_standard(crt_video_standard_t standard,
                                               uint16_t active_line_index, size_t pixel_index)
{
    static const uint8_t k_glyph_n[7] = {
        0x11U, 0x19U, 0x15U, 0x13U, 0x11U, 0x11U, 0x11U,
    };
    static const uint8_t k_glyph_p[7] = {
        0x1eU, 0x11U, 0x11U, 0x1eU, 0x10U, 0x10U, 0x10U,
    };
    const uint8_t *glyph = (standard == CRT_VIDEO_STANDARD_NTSC) ? k_glyph_n : k_glyph_p;
    const uint16_t y_scale = 2U;
    const uint16_t x_scale = 2U;
    const uint16_t glyph_h = 7U * y_scale;
    const uint16_t glyph_w = 5U * x_scale;
    const uint16_t x0 = 8U;

    if (active_line_index >= glyph_h) {
        return false;
    }
    if (pixel_index < x0 || pixel_index >= (size_t)(x0 + glyph_w)) {
        return false;
    }

    const uint16_t row = active_line_index / y_scale;
    const uint16_t col = (uint16_t)(pixel_index - x0) / x_scale;
    return (glyph[row] & (uint8_t)(1U << (4U - col))) != 0U;
}

void crt_demo_pattern_build_color_bars_row(uint8_t *pixels, size_t width)
{
    const size_t bar_count = 8U;

    for (size_t i = 0; i < width; ++i) {
        size_t bar_index = (i * bar_count) / width;
        pixels[i] = (uint8_t)(bar_index < bar_count ? bar_index : (bar_count - 1U));
    }
}

void crt_demo_pattern_build_grayscale_ramp_row(uint8_t *pixels, size_t width)
{
    for (size_t i = 0; i < width; ++i) {
        pixels[i] = (width <= 1U) ? 0xFFU : (uint8_t)((i * 255U) / (width - 1U));
    }
}

static uint8_t crt_demo_rgb332(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint8_t)(((red & 0x7U) << 5U) | ((green & 0x7U) << 2U) | (blue & 0x3U));
}

static uint8_t crt_demo_rgb332_gray(size_t index, size_t count)
{
    const size_t value = (count <= 1U) ? 255U : (index * 255U) / (count - 1U);
    return crt_demo_rgb332((uint8_t)((value * 7U) / 255U), (uint8_t)((value * 7U) / 255U),
                           (uint8_t)((value * 3U) / 255U));
}

void crt_demo_pattern_fill_rgb332_calibration_card(uint8_t *pixels, size_t width, size_t height,
                                                   size_t stride)
{
    static const uint8_t k_bars[8] = {
        0xffU, /* white */
        0xfcU, /* yellow */
        0x1fU, /* cyan */
        0x1cU, /* green */
        0xe3U, /* magenta */
        0xe0U, /* red */
        0x03U, /* blue */
        0x00U, /* black */
    };
    const size_t bar_height = height / 3U;
    const size_t ramp_height = (height >= 96U) ? 32U : (height / 6U);
    const size_t ramp_start = (height > ramp_height) ? (height - ramp_height) : 0U;
    const size_t safe_inset = (width >= 96U && height >= 96U) ? 8U : 1U;
    const size_t center_x = width / 2U;
    const size_t center_y = height / 2U;
    const uint8_t dark_gray = crt_demo_rgb332(1U, 1U, 1U);
    const uint8_t grid_gray = crt_demo_rgb332(3U, 3U, 1U);

    if (pixels == NULL || width == 0U || height == 0U || stride < width) {
        return;
    }

    for (size_t y = 0; y < height; ++y) {
        uint8_t *row = &pixels[y * stride];
        for (size_t x = 0; x < width; ++x) {
            uint8_t value = dark_gray;
            if (y < bar_height) {
                size_t bar = (x * 8U) / width;
                value = k_bars[bar < 8U ? bar : 7U];
            } else if (y >= ramp_start) {
                value = crt_demo_rgb332_gray(x, width);
            } else {
                if ((x % 32U) == 0U || (y % 24U) == 0U) {
                    value = grid_gray;
                }
                if (((y - bar_height) % 16U) < 8U) {
                    static const uint8_t k_phase_stripes[4] = {0xe0U, 0xfcU, 0x1fU, 0x03U};
                    value = k_phase_stripes[(x / 8U) & 0x3U];
                }
            }

            if (x == center_x || y == center_y) {
                value = 0xffU;
            }
            if ((x >= safe_inset && x + safe_inset < width &&
                 (y == safe_inset || y + safe_inset + 1U == height)) ||
                (y >= safe_inset && y + safe_inset < height &&
                 (x == safe_inset || x + safe_inset + 1U == width))) {
                value = 0xffU;
            }
            row[x] = value;
        }
    }
}

bool crt_demo_pattern_is_ramp_region(const crt_demo_pattern_runtime_t *runtime,
                                     uint16_t active_line_index)
{
    const uint16_t ramp_start =
        (runtime->active_line_count > runtime->ramp_height_lines)
            ? (uint16_t)(runtime->active_line_count - runtime->ramp_height_lines)
            : (uint16_t)0U;

    return active_line_index >= ramp_start;
}

void crt_demo_pattern_runtime_init(crt_demo_pattern_runtime_t *runtime,
                                   crt_demo_pattern_mode_t mode, uint16_t active_line_count)
{
    const size_t safe_margin = CRT_DEMO_PATTERN_LOGICAL_WIDTH / 10U; /* ~10% each side */

    runtime->mode = mode;
    runtime->active_line_count = active_line_count;
    runtime->ramp_height_lines = CRT_DEMO_RAMP_HEIGHT_LINES;

    crt_demo_pattern_build_color_bars_row(runtime->color_bars_row, CRT_DEMO_PATTERN_LOGICAL_WIDTH);
    crt_demo_pattern_build_grayscale_ramp_row(runtime->grayscale_ramp_row,
                                              CRT_DEMO_PATTERN_LOGICAL_WIDTH);

    /* Apply safe area margins — set edges to black so all 8 bars fit within
       the ~80% center of the active window, avoiding CRT overscan cutoff. */
    for (size_t i = 0; i < safe_margin; ++i) {
        runtime->color_bars_row[i] = 7U; /* black */
        runtime->color_bars_row[CRT_DEMO_PATTERN_LOGICAL_WIDTH - 1U - i] = 7U;
        runtime->grayscale_ramp_row[i] = 0U;
        runtime->grayscale_ramp_row[CRT_DEMO_PATTERN_LOGICAL_WIDTH - 1U - i] = 255U;
    }
}

void crt_demo_pattern_render_active_window(const crt_demo_pattern_runtime_t *runtime,
                                           const crt_demo_pattern_render_context_t *ctx,
                                           uint16_t blank_level, uint16_t *samples,
                                           size_t sample_count)
{
    const bool ramp_region = (runtime->mode != CRT_DEMO_PATTERN_DISABLED) &&
                             crt_demo_pattern_is_ramp_region(runtime, ctx->active_line_index);

    (void)blank_level;

    if (runtime->mode == CRT_DEMO_PATTERN_DISABLED || !ctx->in_active) {
        return;
    }

    if (runtime->mode == CRT_DEMO_PATTERN_LUMA_BARS) {
        if (ramp_region) {
            for (size_t i = 0; i < sample_count; ++i) {
                const uint8_t pixel =
                    crt_demo_pixel_for_sample(i, sample_count, runtime->grayscale_ramp_row);
                const uint32_t level =
                    CRT_DEMO_LUMA_BLACK +
                    (((uint32_t)(CRT_DEMO_LUMA_WHITE - CRT_DEMO_LUMA_BLACK) * pixel) / 255U);
                samples[i] = (uint16_t)level;
            }
        } else {
            crt_demo_pattern_render_luma_bars(runtime, samples, sample_count);
        }
    } else {
        if (ramp_region) {
            for (size_t i = 0; i < sample_count; ++i) {
                const uint8_t pixel =
                    crt_demo_pixel_for_sample(i, sample_count, runtime->grayscale_ramp_row);
                const uint32_t level =
                    CRT_DEMO_LUMA_BLACK +
                    (((uint32_t)(CRT_DEMO_LUMA_WHITE - CRT_DEMO_LUMA_BLACK) * pixel) / 255U);
                samples[i] = (uint16_t)level;
            }
        } else {
            crt_demo_pattern_render_color_bars(runtime, ctx, samples, sample_count);
        }
    }

    for (size_t i = 0; i < sample_count; ++i) {
        const size_t pixel_index = (i * CRT_DEMO_PATTERN_LOGICAL_WIDTH) / sample_count;
        if (crt_demo_marker_pixel_for_standard(ctx->video_standard, ctx->active_line_index,
                                               pixel_index)) {
            samples[i] = CRT_DEMO_LUMA_WHITE;
        }
    }
}
