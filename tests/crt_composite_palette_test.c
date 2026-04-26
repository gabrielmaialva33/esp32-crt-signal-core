#include "crt_composite_palette.h"

#include <assert.h>
#include <string.h>

static void test_ntsc_yellow_matches_esp_8_bit_table(void)
{
    uint8_t pixels[4] = {0xFC, 0xFC, 0xFC, 0xFC};
    uint16_t samples[CRT_COMPOSITE_RGB332_SAMPLES_PER_4] = {0};
    const uint16_t expected[CRT_COMPOSITE_RGB332_SAMPLES_PER_4] = {
        0x3a00, 0x4100, 0x4c00, 0x4500, 0x3a00, 0x4100,
        0x4c00, 0x4500, 0x3a00, 0x4100, 0x4c00, 0x4500,
    };

    assert(crt_composite_rgb332_packed(CRT_VIDEO_STANDARD_NTSC, 0, 0xFC) == 0x454C413AU);
    crt_composite_rgb332_encode_quad(CRT_VIDEO_STANDARD_NTSC, 0, pixels, samples);
    assert(memcmp(samples, expected, sizeof(expected)) == 0);
}

static void test_pal_yellow_phase_alternates_by_line(void)
{
    uint8_t pixels[4] = {0xFC, 0xFC, 0xFC, 0xFC};
    uint16_t even_samples[CRT_COMPOSITE_RGB332_SAMPLES_PER_4] = {0};
    uint16_t odd_samples[CRT_COMPOSITE_RGB332_SAMPLES_PER_4] = {0};
    const uint16_t expected_even[CRT_COMPOSITE_RGB332_SAMPLES_PER_4] = {
        0x3500, 0x4000, 0x5100, 0x4700, 0x3500, 0x4000,
        0x5100, 0x4700, 0x3500, 0x4000, 0x5100, 0x4700,
    };
    const uint16_t expected_odd[CRT_COMPOSITE_RGB332_SAMPLES_PER_4] = {
        0x3500, 0x4700, 0x5100, 0x4000, 0x3500, 0x4700,
        0x5100, 0x4000, 0x3500, 0x4700, 0x5100, 0x4000,
    };

    assert(crt_composite_rgb332_packed(CRT_VIDEO_STANDARD_PAL, 0, 0xFC) == 0x47514035U);
    assert(crt_composite_rgb332_packed(CRT_VIDEO_STANDARD_PAL, 1, 0xFC) == 0x40514735U);
    crt_composite_rgb332_encode_quad(CRT_VIDEO_STANDARD_PAL, 0, pixels, even_samples);
    crt_composite_rgb332_encode_quad(CRT_VIDEO_STANDARD_PAL, 1, pixels, odd_samples);
    assert(memcmp(even_samples, expected_even, sizeof(expected_even)) == 0);
    assert(memcmp(odd_samples, expected_odd, sizeof(expected_odd)) == 0);
}

static void test_render_full_rgb332_line(void)
{
    uint8_t row[CRT_COMPOSITE_RGB332_WIDTH] = {0};
    uint16_t samples[CRT_COMPOSITE_RGB332_ACTIVE_WIDTH] = {0};

    memset(row, 0xFF, sizeof(row));
    crt_composite_rgb332_render_256_to_768(CRT_VIDEO_STANDARD_NTSC, 0, row, samples);

    for (size_t i = 0; i < CRT_COMPOSITE_RGB332_ACTIVE_WIDTH; ++i) {
        assert(samples[i] == 0x4900);
    }
}

int main(void)
{
    test_ntsc_yellow_matches_esp_8_bit_table();
    test_pal_yellow_phase_alternates_by_line();
    test_render_full_rgb332_line();
    return 0;
}
