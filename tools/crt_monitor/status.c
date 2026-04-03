#include "status.h"
#include <stdio.h>
#include <time.h>

static time_t s_start_time = 0;
static const char *s_device = "/dev/video0";

void status_init(const char *device)
{
    s_start_time = time(NULL);
    s_device = device;
}

size_t status_to_json(char *buf, size_t buf_size)
{
    long uptime = (long)(time(NULL) - s_start_time);
    return (size_t)snprintf(buf, buf_size,
        "{\"video_standard\":\"NTSC\",\"color_enabled\":true,"
        "\"active_lines\":240,\"sample_rate_hz\":14318180,"
        "\"capture_device\":\"%s\","
        "\"capture_resolution\":\"1280x720\","
        "\"uptime_s\":%ld}", s_device, uptime);
}
