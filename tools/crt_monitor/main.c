#include "mongoose.h"
#include <signal.h>
#include <stdio.h>

static volatile sig_atomic_t s_shutdown = 0;

static void signal_handler(int sig)
{
    (void)sig;
    s_shutdown = 1;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        (void)hm;
        mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "CRT Monitor OK\n");
    }
}

int main(int argc, char *argv[])
{
    struct mg_mgr mgr;
    const char *listen_url = argc > 1 ? argv[1] : "http://0.0.0.0:8080";

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, listen_url, ev_handler, NULL);
    printf("CRT Monitor listening on %s\n", listen_url);

    while (!s_shutdown) {
        mg_mgr_poll(&mgr, 100);
    }

    mg_mgr_free(&mgr);
    printf("Shutdown.\n");
    return 0;
}
