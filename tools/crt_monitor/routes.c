#include "routes.h"
void routes_init(app_ctx_t *app, capture_ctx_t *capture,
                 const char *static_dir, const char *captures_dir)
{
    (void)app; (void)capture; (void)static_dir; (void)captures_dir;
}

void routes_handle(struct mg_connection *c, int ev, void *ev_data)
{
    (void)c; (void)ev; (void)ev_data;
}

void routes_broadcast_frame(struct mg_mgr *mgr, const uint8_t *jpg, size_t len)
{
    (void)mgr; (void)jpg; (void)len;
}
