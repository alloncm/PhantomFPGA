# PhantomFPGA Viewer - ASCII Animation Terminal Client
#
# Simple build - no cross-compilation needed.
# This runs on the host machine, not in the VM.

CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11
LDFLAGS =

TARGET = phantomfpga_view
SRCS = phantomfpga_view.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

# For debugging
debug: CFLAGS += -g -O0
debug: clean $(TARGET)
