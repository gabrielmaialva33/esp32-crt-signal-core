#ifndef CRT_COMPOSE_LAYERS_H
#define CRT_COMPOSE_LAYERS_H

#include "crt_compose.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file crt_compose_layers.h
 * @brief Built-in indexed-8 layer fetchers for crt_compose.
 *
 * These helpers cover common compositor primitives so applications do not need
 * custom callbacks for every solid fill, rectangle, or checker pattern.
 */

typedef struct {
    uint8_t fill_idx;
} crt_compose_solid_layer_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t fill_idx;
    uint8_t transparent_idx;
} crt_compose_rect_layer_t;

typedef struct {
    uint8_t first_idx;
    uint8_t second_idx;
    uint8_t cell_w;
    uint8_t cell_h;
} crt_compose_checker_layer_t;

typedef struct {
    crt_layer_fetch_fn source_fetch;
    void *source_ctx;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t viewport_x;
    uint16_t viewport_y;
    uint16_t viewport_width;
    uint16_t viewport_height;
    int32_t scroll_x;
    int32_t scroll_y;
    uint8_t transparent_idx;
    uint8_t scratch[CRT_COMPOSE_MAX_WIDTH];
} crt_compose_viewport_layer_t;

void crt_compose_solid_layer_init(crt_compose_solid_layer_t *layer, uint8_t fill_idx);
bool crt_compose_solid_layer_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                   uint16_t width);

void crt_compose_rect_layer_init(crt_compose_rect_layer_t *layer, uint16_t x, uint16_t y,
                                 uint16_t width, uint16_t height, uint8_t fill_idx,
                                 uint8_t transparent_idx);
void crt_compose_rect_layer_set_bounds(crt_compose_rect_layer_t *layer, uint16_t x, uint16_t y,
                                       uint16_t width, uint16_t height);
void crt_compose_rect_layer_set_fill(crt_compose_rect_layer_t *layer, uint8_t fill_idx);
bool crt_compose_rect_layer_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                  uint16_t width);

void crt_compose_checker_layer_init(crt_compose_checker_layer_t *layer, uint8_t first_idx,
                                    uint8_t second_idx, uint8_t cell_w, uint8_t cell_h);
bool crt_compose_checker_layer_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                     uint16_t width);

void crt_compose_viewport_layer_init(crt_compose_viewport_layer_t *layer,
                                     crt_layer_fetch_fn source_fetch, void *source_ctx,
                                     uint16_t source_width, uint16_t source_height,
                                     uint8_t transparent_idx);
void crt_compose_viewport_layer_set_source(crt_compose_viewport_layer_t *layer,
                                           crt_layer_fetch_fn source_fetch, void *source_ctx,
                                           uint16_t source_width, uint16_t source_height);
void crt_compose_viewport_layer_set_viewport(crt_compose_viewport_layer_t *layer, uint16_t x,
                                             uint16_t y, uint16_t width, uint16_t height);
void crt_compose_viewport_layer_set_scroll(crt_compose_viewport_layer_t *layer, int32_t x,
                                           int32_t y);
void crt_compose_viewport_layer_scroll_by(crt_compose_viewport_layer_t *layer, int32_t dx,
                                          int32_t dy);
bool crt_compose_viewport_layer_fetch(void *ctx, uint16_t logical_line, uint8_t *idx_out,
                                      uint16_t width);

#ifdef __cplusplus
}
#endif

#endif /* CRT_COMPOSE_LAYERS_H */
