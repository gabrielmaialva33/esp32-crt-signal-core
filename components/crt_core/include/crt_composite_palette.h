#pragma once

#include "crt_timing.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRT_COMPOSITE_RGB332_WIDTH         256U
#define CRT_COMPOSITE_RGB332_ACTIVE_WIDTH  768U
#define CRT_COMPOSITE_RGB332_SAMPLES_PER_4 12U

uint32_t crt_composite_rgb332_packed(crt_video_standard_t standard, uint16_t line_index,
                                     uint8_t rgb332);
void crt_composite_rgb332_encode_quad(crt_video_standard_t standard, uint16_t line_index,
                                      const uint8_t rgb332[4],
                                      uint16_t dst[CRT_COMPOSITE_RGB332_SAMPLES_PER_4]);
void crt_composite_rgb332_render_256_to_768(crt_video_standard_t standard, uint16_t line_index,
                                            const uint8_t src[CRT_COMPOSITE_RGB332_WIDTH],
                                            uint16_t dst[CRT_COMPOSITE_RGB332_ACTIVE_WIDTH]);

#ifdef __cplusplus
}
#endif
