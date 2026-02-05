# PhantomFPGA App - Cross-compilation wrapper for CMake
#
# Usage:
#   make          - Build using Buildroot toolchain (auto-detected)
#   make clean    - Clean build artifacts
#   make help     - Show this help
#
# The Makefile auto-detects the Buildroot toolchain and calls CMake
# with the right cross-compilation settings. Just run "make".

APP_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(realpath $(APP_DIR)/..)
BR_OUTPUT := $(PROJECT_ROOT)/platform/buildroot/output
BUILD_DIR := $(APP_DIR)/build

HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH),aarch64)
  TARGET_ARCH := aarch64
  CMAKE_SYSTEM_PROCESSOR := aarch64
  CROSS_PREFIX := aarch64-buildroot-linux-gnu
else ifeq ($(HOST_ARCH),arm64)
  TARGET_ARCH := aarch64
  CMAKE_SYSTEM_PROCESSOR := aarch64
  CROSS_PREFIX := aarch64-buildroot-linux-gnu
else
  TARGET_ARCH := x86_64
  CMAKE_SYSTEM_PROCESSOR := x86_64
  CROSS_PREFIX := x86_64-buildroot-linux-gnu
endif

BR_ARCH_OUTPUT := $(BR_OUTPUT)/$(TARGET_ARCH)
BR_TOOLCHAIN := $(BR_ARCH_OUTPUT)/host/bin/$(CROSS_PREFIX)
TOOLCHAIN_FILE := $(BUILD_DIR)/toolchain.cmake

.PHONY: all clean help

all: $(BUILD_DIR)/phantomfpga_app

$(BUILD_DIR)/phantomfpga_app: $(wildcard $(APP_DIR)/*.c) $(wildcard $(APP_DIR)/../driver/*.h)
	@# Generate CMake toolchain file for cross-compilation
	@mkdir -p $(BUILD_DIR)
	@if [ -x "$(BR_TOOLCHAIN)-gcc" ]; then \
		echo "set(CMAKE_SYSTEM_NAME Linux)" > $(TOOLCHAIN_FILE); \
		echo "set(CMAKE_SYSTEM_PROCESSOR $(CMAKE_SYSTEM_PROCESSOR))" >> $(TOOLCHAIN_FILE); \
		echo "set(CMAKE_C_COMPILER $(BR_TOOLCHAIN)-gcc)" >> $(TOOLCHAIN_FILE); \
		echo "set(CMAKE_FIND_ROOT_PATH $(BR_ARCH_OUTPUT)/host/$(CROSS_PREFIX)/sysroot)" >> $(TOOLCHAIN_FILE); \
		echo "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)" >> $(TOOLCHAIN_FILE); \
		echo "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)" >> $(TOOLCHAIN_FILE); \
		echo "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)" >> $(TOOLCHAIN_FILE); \
		cd $(BUILD_DIR) && cmake $(APP_DIR) -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE); \
	else \
		echo "Buildroot toolchain not found, using host compiler"; \
		cd $(BUILD_DIR) && cmake $(APP_DIR); \
	fi
	@$(MAKE) -C $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "PhantomFPGA App Build"
	@echo ""
	@echo "  make       - Build (auto-detects Buildroot toolchain)"
	@echo "  make clean - Remove build directory"
	@echo ""
	@echo "  Toolchain: $(BR_TOOLCHAIN)-gcc"
	@echo "  Build dir: $(BUILD_DIR)"
