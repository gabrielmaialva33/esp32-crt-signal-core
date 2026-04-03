CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu11 -D_GNU_SOURCE -I. -DMG_ENABLE_LINES=0
LDFLAGS = -lpthread

SRCS = main.c capture.c routes.c status.c mongoose.c
TARGET = crt_monitor

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) capture.h routes.h status.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
