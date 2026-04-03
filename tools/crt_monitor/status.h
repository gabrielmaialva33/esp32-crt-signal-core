#ifndef STATUS_H
#define STATUS_H
#include <stddef.h>
void status_init(const char *device);
size_t status_to_json(char *buf, size_t buf_size);
#endif
