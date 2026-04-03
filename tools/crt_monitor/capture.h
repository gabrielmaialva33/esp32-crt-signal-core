#ifndef CAPTURE_H
#define CAPTURE_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CAPTURE_NUM_BUFFERS 4
#define CAPTURE_DEFAULT_WIDTH 1280
#define CAPTURE_DEFAULT_HEIGHT 720

typedef struct {
    void *start;
    size_t length;
} capture_buffer_t;

typedef struct {
    int fd;
    capture_buffer_t buffers[CAPTURE_NUM_BUFFERS];
    int buffer_count;
    bool streaming;
} capture_ctx_t;

int capture_open(capture_ctx_t *ctx, const char *device, int width, int height);
int capture_grab(capture_ctx_t *ctx, const uint8_t **jpg_buf, size_t *jpg_len);
void capture_close(capture_ctx_t *ctx);
#endif
