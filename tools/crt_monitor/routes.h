#ifndef ROUTES_H
#define ROUTES_H
#include "mongoose.h"
#include "capture.h"

typedef struct {
    capture_ctx_t *capture;
    const char *static_dir;
    const char *captures_dir;
    int ws_client_count;
} app_ctx_t;

void routes_init(app_ctx_t *app, capture_ctx_t *capture,
                 const char *static_dir, const char *captures_dir);
void routes_handle(struct mg_connection *c, int ev, void *ev_data);
void routes_broadcast_frame(struct mg_mgr *mgr, const uint8_t *jpg, size_t len);
#endif
