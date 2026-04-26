/*
 * capture_rtsp.c — RTSP capture backend using ffmpeg subprocess
 *
 * Replaces V4L2 capture with RTSP stream from IP camera.
 * Uses ffmpeg to decode H.265 RTSP and pipe MJPEG frames.
 * Same API as capture.c (capture_open/capture_grab/capture_close).
 *
 * For Yoosee cameras with buggy RTSP (CSeq mismatch on OPTIONS),
 * we use a custom RTSP handshake via helper script.
 */

#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>

/* Double-buffered JPEG frame storage */
#define RTSP_MAX_FRAME_SIZE (2 * 1024 * 1024) /* 2 MB max per JPEG */

typedef struct {
    uint8_t  *data;
    size_t    len;
} frame_buf_t;

static struct {
    FILE       *pipe;
    pid_t       pid;
    pthread_t   reader_thread;
    bool        running;

    /* Double buffer: reader writes to back, grab reads from front */
    frame_buf_t frames[2];
    int         write_idx;     /* reader writes here */
    int         read_idx;      /* grab reads here */
    pthread_mutex_t lock;
    bool        frame_ready;
} s_rtsp;

/* ------------------------------------------------------------------ */
/* MJPEG frame reader thread                                           */
/* ------------------------------------------------------------------ */

/* Read exactly n bytes from pipe */
static bool pipe_read_exact(FILE *f, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        size_t r = fread(buf + got, 1, n - got, f);
        if (r == 0) return false;
        got += r;
    }
    return true;
}

/*
 * ffmpeg outputs raw MJPEG: each frame is a complete JPEG
 * (starts with FF D8, ends with FF D9).
 * We scan for SOI/EOI markers to extract individual frames.
 */
static void *reader_thread_fn(void *arg)
{
    (void)arg;
    uint8_t *scan_buf = malloc(RTSP_MAX_FRAME_SIZE);
    if (!scan_buf) return NULL;

    size_t pos = 0;
    bool in_frame = false;

    while (s_rtsp.running && s_rtsp.pipe) {
        int c = fgetc(s_rtsp.pipe);
        if (c == EOF) break;

        if (pos >= RTSP_MAX_FRAME_SIZE) {
            /* Frame too large, reset */
            pos = 0;
            in_frame = false;
            continue;
        }

        scan_buf[pos++] = (uint8_t)c;

        /* Detect JPEG SOI (FF D8) */
        if (!in_frame && pos >= 2 &&
            scan_buf[pos-2] == 0xFF && scan_buf[pos-1] == 0xD8) {
            /* Start of new frame — reset to just the SOI marker */
            scan_buf[0] = 0xFF;
            scan_buf[1] = 0xD8;
            pos = 2;
            in_frame = true;
            continue;
        }

        /* Detect JPEG EOI (FF D9) */
        if (in_frame && pos >= 2 &&
            scan_buf[pos-2] == 0xFF && scan_buf[pos-1] == 0xD9) {
            /* Complete frame — swap to front buffer */
            pthread_mutex_lock(&s_rtsp.lock);
            int wi = s_rtsp.write_idx;
            if (s_rtsp.frames[wi].data == NULL) {
                s_rtsp.frames[wi].data = malloc(RTSP_MAX_FRAME_SIZE);
            }
            if (s_rtsp.frames[wi].data) {
                memcpy(s_rtsp.frames[wi].data, scan_buf, pos);
                s_rtsp.frames[wi].len = pos;
                /* Swap indices */
                s_rtsp.read_idx = wi;
                s_rtsp.write_idx = 1 - wi;
                s_rtsp.frame_ready = true;
            }
            pthread_mutex_unlock(&s_rtsp.lock);

            pos = 0;
            in_frame = false;
        }
    }

    free(scan_buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int capture_open(capture_ctx_t *ctx, const char *device, int width, int height)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    memset(&s_rtsp, 0, sizeof(s_rtsp));
    pthread_mutex_init(&s_rtsp.lock, NULL);

    /*
     * 'device' is the RTSP URL when using this backend.
     * e.g. "rtsp://admin:pass@192.168.0.100:554/onvif1"
     *
     * We use a Python helper to handle the Yoosee RTSP handshake
     * (skipping OPTIONS), then pipe MJPEG to stdout.
     */

    /* Build ffmpeg command that reads RTSP and outputs MJPEG to stdout.
     * -rtsp_transport tcp avoids UDP packet loss.
     * -vf scale=WxH resizes to requested dimensions.
     * For Yoosee cameras, we use a wrapper script that does the RTSP
     * handshake manually, but first try plain ffmpeg. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "exec python3 -u -c \""
        "import cv2, sys, struct, os;"
        "os.environ['OPENCV_FFMPEG_CAPTURE_OPTIONS']='rtsp_transport;udp';"
        "cap=cv2.VideoCapture('%s',cv2.CAP_FFMPEG);"
        "w,h=%d,%d;"
        "sys.stderr.write(f'[rtsp] opened {cap.isOpened()} -> resize {w}x{h}\\n');"
        "sys.stderr.flush();"
        "import time;"
        "while cap.isOpened():"
        "  ret,frame=cap.read();"
        "  if not ret: time.sleep(0.01); continue;"
        "  frame=cv2.resize(frame,(w,h));"
        "  ok,jpg=cv2.imencode('.jpg',frame,[cv2.IMWRITE_JPEG_QUALITY,85]);"
        "  if ok: sys.stdout.buffer.write(jpg.tobytes()); sys.stdout.buffer.flush();"
        "\"",
        device, width, height);

    fprintf(stderr, "[capture_rtsp] starting RTSP reader for %s (%dx%d)\n",
            device, width, height);

    s_rtsp.pipe = popen(cmd, "r");
    if (!s_rtsp.pipe) {
        fprintf(stderr, "[capture_rtsp] popen failed: %s\n", strerror(errno));
        return -1;
    }

    s_rtsp.running = true;
    s_rtsp.write_idx = 0;
    s_rtsp.read_idx = -1;

    if (pthread_create(&s_rtsp.reader_thread, NULL, reader_thread_fn, NULL) != 0) {
        fprintf(stderr, "[capture_rtsp] pthread_create failed\n");
        pclose(s_rtsp.pipe);
        s_rtsp.pipe = NULL;
        return -1;
    }

    /* Wait a bit for first frame */
    for (int i = 0; i < 50; i++) { /* 5 seconds max */
        usleep(100000);
        if (s_rtsp.frame_ready) break;
    }

    if (s_rtsp.frame_ready) {
        fprintf(stderr, "[capture_rtsp] first frame received, streaming active\n");
    } else {
        fprintf(stderr, "[capture_rtsp] warning: no frame yet, stream may be slow to start\n");
    }

    ctx->fd = 1; /* dummy non-negative to signal "open" */
    ctx->streaming = true;
    return 0;
}

int capture_grab(capture_ctx_t *ctx, const uint8_t **jpg_buf, size_t *jpg_len)
{
    (void)ctx;

    pthread_mutex_lock(&s_rtsp.lock);
    if (!s_rtsp.frame_ready || s_rtsp.read_idx < 0) {
        pthread_mutex_unlock(&s_rtsp.lock);
        return -1;
    }

    int ri = s_rtsp.read_idx;
    *jpg_buf = s_rtsp.frames[ri].data;
    *jpg_len = s_rtsp.frames[ri].len;
    pthread_mutex_unlock(&s_rtsp.lock);

    return (*jpg_buf && *jpg_len > 0) ? 0 : -1;
}

void capture_close(capture_ctx_t *ctx)
{
    s_rtsp.running = false;

    if (s_rtsp.pipe) {
        pclose(s_rtsp.pipe);
        s_rtsp.pipe = NULL;
    }

    pthread_join(s_rtsp.reader_thread, NULL);

    for (int i = 0; i < 2; i++) {
        free(s_rtsp.frames[i].data);
        s_rtsp.frames[i].data = NULL;
    }

    pthread_mutex_destroy(&s_rtsp.lock);
    ctx->fd = -1;
    ctx->streaming = false;
    fprintf(stderr, "[capture_rtsp] closed\n");
}
