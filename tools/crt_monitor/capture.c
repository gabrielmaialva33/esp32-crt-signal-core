#include "capture.h"
#include <stdio.h>

int capture_open(capture_ctx_t *ctx, const char *device, int width, int height)
{
    (void)ctx; (void)device; (void)width; (void)height;
    fprintf(stderr, "[capture] stub: not implemented\n");
    return -1;
}

int capture_grab(capture_ctx_t *ctx, const uint8_t **jpg_buf, size_t *jpg_len)
{
    (void)ctx; (void)jpg_buf; (void)jpg_len;
    return -1;
}

void capture_close(capture_ctx_t *ctx) { (void)ctx; }
