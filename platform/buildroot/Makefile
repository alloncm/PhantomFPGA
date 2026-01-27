# PhantomFPGA Buildroot Makefile
# Builds a minimal Linux guest for FPGA driver development training
#
# Usage:
#   make              - Build x86_64 image (default)
#   make aarch64      - Build aarch64 image
#   make all-arches   - Build both architectures
#   make help         - Show available targets
#
# The build process downloads Buildroot, applies our defconfig, copies
# overlay files, and produces kernel + rootfs for QEMU.
#
# "Building embedded Linux images is like making a soufle: lots of waiting,
# and you really don't want it to collapse at the end." -- Unknown Embedded Dev

# Configuration
BUILDROOT_VERSION := 2024.02.10
BUILDROOT_TARBALL := buildroot-$(BUILDROOT_VERSION).tar.gz
BUILDROOT_URL := https://buildroot.org/downloads/$(BUILDROOT_TARBALL)
BUILDROOT_SHA256 := c2bfdd1d81d3a7f4e7c51458e0d7bc13b4f8c9cb13b99b6e20b9dd0b5d7e6a5f

# Directories (relative to this Makefile)
MAKEFILE_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
TOP_DIR := $(realpath $(MAKEFILE_DIR)/../..)
BR_DIR := $(MAKEFILE_DIR)/buildroot
OUTPUT_X86 := $(MAKEFILE_DIR)/output/x86_64
OUTPUT_ARM := $(MAKEFILE_DIR)/output/aarch64
DL_DIR := $(MAKEFILE_DIR)/dl
CONFIG_DIR := $(MAKEFILE_DIR)/configs
OVERLAY_DIR := $(MAKEFILE_DIR)/overlay
IMAGES_DIR := $(TOP_DIR)/platform/images

# Defconfig names
DEFCONFIG_X86 := phantomfpga_x86_64_defconfig
DEFCONFIG_ARM := phantomfpga_aarch64_defconfig

# Build parallelism (use all cores by default, user can override)
NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Timestamp files for incremental builds
STAMP_DIR := $(MAKEFILE_DIR)/.stamps
STAMP_DOWNLOAD := $(STAMP_DIR)/download
STAMP_EXTRACT := $(STAMP_DIR)/extract
STAMP_CONFIG_X86 := $(STAMP_DIR)/config-x86_64
STAMP_CONFIG_ARM := $(STAMP_DIR)/config-aarch64
STAMP_BUILD_X86 := $(STAMP_DIR)/build-x86_64
STAMP_BUILD_ARM := $(STAMP_DIR)/build-aarch64

# Output files
KERNEL_X86 := bzImage
KERNEL_ARM := Image
ROOTFS := rootfs.ext4

# Terminal colors for pretty output (if stdout is a tty)
ifneq ($(TERM),)
  GREEN := \033[0;32m
  YELLOW := \033[0;33m
  RED := \033[0;31m
  BLUE := \033[0;34m
  NC := \033[0m
else
  GREEN :=
  YELLOW :=
  RED :=
  BLUE :=
  NC :=
endif

# Verbose mode - set V=1 to see all commands
V ?= 0
ifeq ($(V),1)
  Q :=
  QUIET :=
else
  Q := @
  QUIET := --quiet
endif

#==============================================================================
# Default target: build x86_64
#==============================================================================
.PHONY: all
all: x86_64

#==============================================================================
# Architecture targets
#==============================================================================
.PHONY: x86_64
x86_64: $(STAMP_BUILD_X86)
	@echo "$(GREEN)==> x86_64 build complete!$(NC)"
	@echo "    Kernel: $(IMAGES_DIR)/bzImage"
	@echo "    Rootfs: $(IMAGES_DIR)/rootfs.ext4"

.PHONY: aarch64
aarch64: $(STAMP_BUILD_ARM)
	@echo "$(GREEN)==> aarch64 build complete!$(NC)"
	@echo "    Kernel: $(IMAGES_DIR)/Image"
	@echo "    Rootfs: $(IMAGES_DIR)/rootfs-aarch64.ext4"

.PHONY: all-arches
all-arches: x86_64 aarch64
	@echo "$(GREEN)==> All architectures built successfully!$(NC)"

#==============================================================================
# Download Buildroot tarball
#==============================================================================
.PHONY: download
download: $(STAMP_DOWNLOAD)

$(STAMP_DOWNLOAD): | $(DL_DIR) $(STAMP_DIR)
	@echo "$(BLUE)==> Downloading Buildroot $(BUILDROOT_VERSION)...$(NC)"
	$(Q)if [ ! -f "$(DL_DIR)/$(BUILDROOT_TARBALL)" ]; then \
		wget $(QUIET) -O "$(DL_DIR)/$(BUILDROOT_TARBALL)" "$(BUILDROOT_URL)" || \
		curl -L $(if $(filter 0,$(V)),-s,) -o "$(DL_DIR)/$(BUILDROOT_TARBALL)" "$(BUILDROOT_URL)"; \
	else \
		echo "    Using cached tarball"; \
	fi
	@touch $@

#==============================================================================
# Extract Buildroot
#==============================================================================
$(STAMP_EXTRACT): $(STAMP_DOWNLOAD)
	@echo "$(BLUE)==> Extracting Buildroot...$(NC)"
	$(Q)rm -rf "$(BR_DIR)"
	$(Q)mkdir -p "$(BR_DIR)"
	$(Q)tar -xzf "$(DL_DIR)/$(BUILDROOT_TARBALL)" -C "$(MAKEFILE_DIR)" --strip-components=1 \
		--one-top-level=buildroot
	@touch $@

#==============================================================================
# Configure for x86_64
#==============================================================================
.PHONY: configure-x86_64
configure-x86_64: $(STAMP_CONFIG_X86)

$(STAMP_CONFIG_X86): $(STAMP_EXTRACT) | $(OUTPUT_X86)
	@echo "$(BLUE)==> Configuring x86_64 build...$(NC)"
	$(Q)cp "$(CONFIG_DIR)/$(DEFCONFIG_X86)" "$(BR_DIR)/configs/"
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" $(DEFCONFIG_X86) \
		BR2_EXTERNAL= \
		BR2_DL_DIR="$(DL_DIR)" \
		$(if $(filter 0,$(V)),>/dev/null 2>&1,)
	@# Patch config to use our local overlay directly
	$(Q)sed -i 's|BR2_ROOTFS_OVERLAY=.*|BR2_ROOTFS_OVERLAY="$(OVERLAY_DIR)"|' \
		"$(OUTPUT_X86)/.config"
	@# Remove BR2_EXTERNAL references since we're not using external tree
	$(Q)sed -i '/BR2_EXTERNAL/d' "$(OUTPUT_X86)/.config"
	$(Q)sed -i 's|$$(BR2_EXTERNAL_PHANTOMFPGA_PATH)/||g' "$(OUTPUT_X86)/.config"
	@# Use default kernel config (we'll customize later if needed)
	$(Q)sed -i 's|BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG=y|BR2_LINUX_KERNEL_USE_DEFCONFIG=y|' \
		"$(OUTPUT_X86)/.config"
	$(Q)sed -i 's|BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=.*|BR2_LINUX_KERNEL_DEFCONFIG_NAME="x86_64_defconfig"|' \
		"$(OUTPUT_X86)/.config"
	@touch $@

#==============================================================================
# Configure for aarch64
#==============================================================================
.PHONY: configure-aarch64
configure-aarch64: $(STAMP_CONFIG_ARM)

$(STAMP_CONFIG_ARM): $(STAMP_EXTRACT) | $(OUTPUT_ARM)
	@echo "$(BLUE)==> Configuring aarch64 build...$(NC)"
	$(Q)cp "$(CONFIG_DIR)/$(DEFCONFIG_ARM)" "$(BR_DIR)/configs/"
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" $(DEFCONFIG_ARM) \
		BR2_EXTERNAL= \
		BR2_DL_DIR="$(DL_DIR)" \
		$(if $(filter 0,$(V)),>/dev/null 2>&1,)
	@# Patch config to use our local overlay directly
	$(Q)sed -i 's|BR2_ROOTFS_OVERLAY=.*|BR2_ROOTFS_OVERLAY="$(OVERLAY_DIR)"|' \
		"$(OUTPUT_ARM)/.config"
	@# Remove BR2_EXTERNAL references since we're not using external tree
	$(Q)sed -i '/BR2_EXTERNAL/d' "$(OUTPUT_ARM)/.config"
	$(Q)sed -i 's|$$(BR2_EXTERNAL_PHANTOMFPGA_PATH)/||g' "$(OUTPUT_ARM)/.config"
	@# Use default kernel config
	$(Q)sed -i 's|BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG=y|BR2_LINUX_KERNEL_USE_DEFCONFIG=y|' \
		"$(OUTPUT_ARM)/.config"
	$(Q)sed -i 's|BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=.*|BR2_LINUX_KERNEL_DEFCONFIG_NAME="defconfig"|' \
		"$(OUTPUT_ARM)/.config"
	@touch $@

#==============================================================================
# Build x86_64
#==============================================================================
.PHONY: build-x86_64
build-x86_64: $(STAMP_BUILD_X86)

$(STAMP_BUILD_X86): $(STAMP_CONFIG_X86) | $(IMAGES_DIR)
	@echo "$(BLUE)==> Building x86_64 image (this will take a while)...$(NC)"
	@echo "    Using $(NPROC) parallel jobs"
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" -j$(NPROC) \
		BR2_DL_DIR="$(DL_DIR)" \
		$(if $(filter 0,$(V)),2>&1 | grep -E "^>>>" || true,)
	@echo "$(BLUE)==> Installing x86_64 images...$(NC)"
	$(Q)cp "$(OUTPUT_X86)/images/$(KERNEL_X86)" "$(IMAGES_DIR)/"
	$(Q)cp "$(OUTPUT_X86)/images/$(ROOTFS)" "$(IMAGES_DIR)/"
	@touch $@

#==============================================================================
# Build aarch64
#==============================================================================
.PHONY: build-aarch64
build-aarch64: $(STAMP_BUILD_ARM)

$(STAMP_BUILD_ARM): $(STAMP_CONFIG_ARM) | $(IMAGES_DIR)
	@echo "$(BLUE)==> Building aarch64 image (this will take a while)...$(NC)"
	@echo "    Using $(NPROC) parallel jobs"
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" -j$(NPROC) \
		BR2_DL_DIR="$(DL_DIR)" \
		$(if $(filter 0,$(V)),2>&1 | grep -E "^>>>" || true,)
	@echo "$(BLUE)==> Installing aarch64 images...$(NC)"
	$(Q)cp "$(OUTPUT_ARM)/images/$(KERNEL_ARM)" "$(IMAGES_DIR)/"
	$(Q)cp "$(OUTPUT_ARM)/images/$(ROOTFS)" "$(IMAGES_DIR)/rootfs-aarch64.ext4"
	@touch $@

#==============================================================================
# Convenience targets
#==============================================================================
.PHONY: configure
configure: configure-x86_64

.PHONY: build
build: build-x86_64

#==============================================================================
# Menuconfig targets (for manual configuration)
#==============================================================================
.PHONY: menuconfig
menuconfig: menuconfig-x86_64

.PHONY: menuconfig-x86_64
menuconfig-x86_64: $(STAMP_CONFIG_X86)
	@echo "$(YELLOW)==> Opening menuconfig for x86_64...$(NC)"
	$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" menuconfig

.PHONY: menuconfig-aarch64
menuconfig-aarch64: $(STAMP_CONFIG_ARM)
	@echo "$(YELLOW)==> Opening menuconfig for aarch64...$(NC)"
	$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" menuconfig

.PHONY: linux-menuconfig
linux-menuconfig: linux-menuconfig-x86_64

.PHONY: linux-menuconfig-x86_64
linux-menuconfig-x86_64: $(STAMP_CONFIG_X86)
	@echo "$(YELLOW)==> Opening Linux kernel menuconfig for x86_64...$(NC)"
	$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" linux-menuconfig

.PHONY: linux-menuconfig-aarch64
linux-menuconfig-aarch64: $(STAMP_CONFIG_ARM)
	@echo "$(YELLOW)==> Opening Linux kernel menuconfig for aarch64...$(NC)"
	$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" linux-menuconfig

#==============================================================================
# Save configuration changes
#==============================================================================
.PHONY: savedefconfig
savedefconfig: savedefconfig-x86_64

.PHONY: savedefconfig-x86_64
savedefconfig-x86_64: $(STAMP_CONFIG_X86)
	@echo "$(YELLOW)==> Saving x86_64 defconfig...$(NC)"
	$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" savedefconfig
	$(Q)cp "$(OUTPUT_X86)/defconfig" "$(CONFIG_DIR)/$(DEFCONFIG_X86)"
	@echo "    Saved to $(CONFIG_DIR)/$(DEFCONFIG_X86)"

.PHONY: savedefconfig-aarch64
savedefconfig-aarch64: $(STAMP_CONFIG_ARM)
	@echo "$(YELLOW)==> Saving aarch64 defconfig...$(NC)"
	$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" savedefconfig
	$(Q)cp "$(OUTPUT_ARM)/defconfig" "$(CONFIG_DIR)/$(DEFCONFIG_ARM)"
	@echo "    Saved to $(CONFIG_DIR)/$(DEFCONFIG_ARM)"

#==============================================================================
# Clean targets
#==============================================================================
.PHONY: clean
clean:
	@echo "$(YELLOW)==> Cleaning build artifacts...$(NC)"
	$(Q)rm -rf "$(OUTPUT_X86)" "$(OUTPUT_ARM)"
	$(Q)rm -f "$(STAMP_CONFIG_X86)" "$(STAMP_CONFIG_ARM)"
	$(Q)rm -f "$(STAMP_BUILD_X86)" "$(STAMP_BUILD_ARM)"
	@echo "    Build directories removed"
	@echo "    Downloads preserved in $(DL_DIR)"

.PHONY: clean-x86_64
clean-x86_64:
	@echo "$(YELLOW)==> Cleaning x86_64 build...$(NC)"
	$(Q)rm -rf "$(OUTPUT_X86)"
	$(Q)rm -f "$(STAMP_CONFIG_X86)" "$(STAMP_BUILD_X86)"

.PHONY: clean-aarch64
clean-aarch64:
	@echo "$(YELLOW)==> Cleaning aarch64 build...$(NC)"
	$(Q)rm -rf "$(OUTPUT_ARM)"
	$(Q)rm -f "$(STAMP_CONFIG_ARM)" "$(STAMP_BUILD_ARM)"

.PHONY: clean-images
clean-images:
	@echo "$(YELLOW)==> Cleaning output images...$(NC)"
	$(Q)rm -f "$(IMAGES_DIR)/bzImage" "$(IMAGES_DIR)/Image"
	$(Q)rm -f "$(IMAGES_DIR)/rootfs.ext4" "$(IMAGES_DIR)/rootfs-aarch64.ext4"

.PHONY: distclean
distclean:
	@echo "$(RED)==> Removing everything (downloads, builds, extracted source)...$(NC)"
	$(Q)rm -rf "$(BR_DIR)" "$(OUTPUT_X86)" "$(OUTPUT_ARM)" "$(DL_DIR)" "$(STAMP_DIR)"
	$(Q)rm -f "$(IMAGES_DIR)/bzImage" "$(IMAGES_DIR)/Image"
	$(Q)rm -f "$(IMAGES_DIR)/rootfs.ext4" "$(IMAGES_DIR)/rootfs-aarch64.ext4"
	@echo "    Everything cleaned. Run 'make' to start fresh."

#==============================================================================
# Info/status targets
#==============================================================================
.PHONY: info
info:
	@echo "$(BLUE)PhantomFPGA Buildroot Configuration$(NC)"
	@echo "====================================="
	@echo "Buildroot version: $(BUILDROOT_VERSION)"
	@echo "Parallel jobs:     $(NPROC)"
	@echo ""
	@echo "Directories:"
	@echo "  Source:     $(BR_DIR)"
	@echo "  Downloads:  $(DL_DIR)"
	@echo "  x86_64 out: $(OUTPUT_X86)"
	@echo "  aarch64 out: $(OUTPUT_ARM)"
	@echo "  Images:     $(IMAGES_DIR)"
	@echo ""
	@echo "Build status:"
	@echo -n "  Download:   "; [ -f "$(STAMP_DOWNLOAD)" ] && echo "done" || echo "pending"
	@echo -n "  Extract:    "; [ -f "$(STAMP_EXTRACT)" ] && echo "done" || echo "pending"
	@echo -n "  x86_64:     "; [ -f "$(STAMP_BUILD_X86)" ] && echo "built" || \
		([ -f "$(STAMP_CONFIG_X86)" ] && echo "configured" || echo "pending")
	@echo -n "  aarch64:    "; [ -f "$(STAMP_BUILD_ARM)" ] && echo "built" || \
		([ -f "$(STAMP_CONFIG_ARM)" ] && echo "configured" || echo "pending")

.PHONY: help
help:
	@echo "$(BLUE)PhantomFPGA Buildroot Build System$(NC)"
	@echo "===================================="
	@echo ""
	@echo "$(GREEN)Build Targets:$(NC)"
	@echo "  make              Build x86_64 image (default)"
	@echo "  make x86_64       Build x86_64 image"
	@echo "  make aarch64      Build aarch64 image"
	@echo "  make all-arches   Build both architectures"
	@echo ""
	@echo "$(GREEN)Individual Steps:$(NC)"
	@echo "  make download     Download Buildroot tarball"
	@echo "  make configure    Configure x86_64 build"
	@echo "  make configure-x86_64    Configure x86_64"
	@echo "  make configure-aarch64   Configure aarch64"
	@echo "  make build        Build x86_64"
	@echo "  make build-x86_64        Build x86_64"
	@echo "  make build-aarch64       Build aarch64"
	@echo ""
	@echo "$(GREEN)Configuration:$(NC)"
	@echo "  make menuconfig           Edit Buildroot config (x86_64)"
	@echo "  make menuconfig-x86_64    Edit Buildroot config (x86_64)"
	@echo "  make menuconfig-aarch64   Edit Buildroot config (aarch64)"
	@echo "  make linux-menuconfig     Edit kernel config (x86_64)"
	@echo "  make linux-menuconfig-x86_64    Edit kernel config (x86_64)"
	@echo "  make linux-menuconfig-aarch64   Edit kernel config (aarch64)"
	@echo "  make savedefconfig        Save current config (x86_64)"
	@echo "  make savedefconfig-x86_64       Save x86_64 config"
	@echo "  make savedefconfig-aarch64      Save aarch64 config"
	@echo ""
	@echo "$(GREEN)Cleaning:$(NC)"
	@echo "  make clean        Remove build artifacts (keep downloads)"
	@echo "  make clean-x86_64        Clean x86_64 only"
	@echo "  make clean-aarch64       Clean aarch64 only"
	@echo "  make clean-images        Remove output images only"
	@echo "  make distclean    Remove everything including downloads"
	@echo ""
	@echo "$(GREEN)Information:$(NC)"
	@echo "  make info         Show current build status"
	@echo "  make help         Show this help"
	@echo ""
	@echo "$(GREEN)Options:$(NC)"
	@echo "  NPROC=N           Use N parallel jobs (default: $(NPROC))"
	@echo "  V=1               Verbose output"
	@echo ""
	@echo "$(GREEN)Output Files:$(NC)"
	@echo "  $(IMAGES_DIR)/bzImage           x86_64 kernel"
	@echo "  $(IMAGES_DIR)/rootfs.ext4       x86_64 rootfs"
	@echo "  $(IMAGES_DIR)/Image             aarch64 kernel"
	@echo "  $(IMAGES_DIR)/rootfs-aarch64.ext4    aarch64 rootfs"
	@echo ""
	@echo "$(YELLOW)Note:$(NC) First build takes 30-60 minutes depending on your machine."
	@echo "      Subsequent builds are incremental and much faster."

#==============================================================================
# Directory creation
#==============================================================================
$(DL_DIR) $(OUTPUT_X86) $(OUTPUT_ARM) $(STAMP_DIR) $(IMAGES_DIR):
	$(Q)mkdir -p $@

#==============================================================================
# Prevent parallel execution of top-level targets
# (Buildroot itself handles parallelism internally)
#==============================================================================
.NOTPARALLEL:

#==============================================================================
# Mark phony targets
#==============================================================================
.PHONY: all x86_64 aarch64 all-arches download configure configure-x86_64 \
        configure-aarch64 build build-x86_64 build-aarch64 menuconfig \
        menuconfig-x86_64 menuconfig-aarch64 linux-menuconfig \
        linux-menuconfig-x86_64 linux-menuconfig-aarch64 savedefconfig \
        savedefconfig-x86_64 savedefconfig-aarch64 clean clean-x86_64 \
        clean-aarch64 clean-images distclean info help
