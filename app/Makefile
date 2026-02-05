# PhantomFPGA Userspace Application
#
# Usage:
#   make          - Build (auto-detects Buildroot cross-compiler)
#   make clean    - Clean build artifacts
#
# Just one C file, one binary. No cmake needed.

APP_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(realpath $(APP_DIR)/..)
BR_OUTPUT := $(PROJECT_ROOT)/platform/buildroot/output

# Auto-detect Buildroot cross-compiler
HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH),aarch64)
  CROSS_PREFIX := aarch64-buildroot-linux-gnu-
else ifeq ($(HOST_ARCH),arm64)
  CROSS_PREFIX := aarch64-buildroot-linux-gnu-
else
  CROSS_PREFIX := x86_64-buildroot-linux-gnu-
endif

TARGET_ARCH := $(if $(filter aarch64 arm64,$(HOST_ARCH)),aarch64,x86_64)
BR_CC := $(BR_OUTPUT)/$(TARGET_ARCH)/host/bin/$(CROSS_PREFIX)gcc

# Use Buildroot compiler if available, otherwise host gcc
CC := $(if $(wildcard $(BR_CC)),$(BR_CC),gcc)

CFLAGS := -Wall -Wextra -O2 -I$(APP_DIR)/../driver
LDFLAGS := -lpthread -lrt

TARGET := phantomfpga_app
SRC := phantomfpga_app.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(wildcard $(APP_DIR)/../driver/*.h)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
