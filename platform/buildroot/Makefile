# =============================================================================
# PhantomFPGA Buildroot Makefile
# =============================================================================
#
# This Makefile builds a minimal Linux system for our QEMU-based training VM.
# It wraps Buildroot, which is a tool for building embedded Linux systems.
#
# STOP! Before you run "make" and walk away, take a moment to understand
# what's happening here. In real embedded work, you'll need to:
#   - Customize kernel configs for your hardware
#   - Add drivers and userspace tools
#   - Debug build failures
#   - Optimize image sizes
#
# This Makefile is your introduction to that world. Read the comments.
#
# =============================================================================
# WHAT IS BUILDROOT?
# =============================================================================
#
# Buildroot (https://buildroot.org) automates building:
#   1. A cross-compiler toolchain (gcc for your target architecture)
#   2. A Linux kernel (configured for your target)
#   3. A root filesystem (with busybox, libraries, your packages)
#
# Why not just use your distro's kernel? Because:
#   - Embedded systems need minimal, customized kernels
#   - You need to cross-compile for different architectures
#   - You control exactly what goes into the image
#
# =============================================================================
# BUILD PROCESS OVERVIEW
# =============================================================================
#
#   Download -> Extract -> Configure -> Build -> Output
#
# 1. DOWNLOAD: Fetch Buildroot tarball (once, cached in dl/)
# 2. EXTRACT:  Unpack to buildroot/ directory
# 3. CONFIGURE: Apply our defconfig (kernel, packages, settings)
# 4. BUILD:    Compile everything (toolchain, kernel, rootfs)
# 5. OUTPUT:   Copy kernel + rootfs to platform/images/
#
# We use "stamp files" (.stamps/) to track completed steps. If a step
# is already done, we skip it. Delete a stamp to force re-running.
#
# =============================================================================
# KEY CONCEPTS
# =============================================================================
#
# DEFCONFIG:
#   A minimal config file listing only non-default options. Buildroot
#   expands this into a full .config. Our defconfigs are in configs/.
#   Edit with "make menuconfig", save with "make savedefconfig".
#
# OVERLAY:
#   Files copied on top of the root filesystem. Our overlay/ directory
#   contains custom configs like /etc/fstab. This is how you add files
#   to the image without patching packages.
#
# OUT-OF-TREE BUILD (O=...):
#   Buildroot outputs go to a separate directory, keeping source clean.
#   We use output/x86_64/ and output/aarch64/ for multi-arch builds.
#
# BR2_* VARIABLES:
#   Buildroot configuration variables. BR2_DL_DIR sets download cache,
#   BR2_ROOTFS_OVERLAY sets overlay path, etc.
#
# =============================================================================
# USAGE
# =============================================================================
#
#   make              - Build image (auto-detects your architecture)
#   make x86_64       - Build x86_64 image explicitly
#   make aarch64      - Build aarch64 image explicitly
#   make all-arches   - Build both architectures
#   make help         - Show all targets
#   make menuconfig   - Edit Buildroot config interactively
#   make info         - Show build status
#
# First build takes 30-60 minutes. Subsequent builds are faster.
#
# =============================================================================

# -----------------------------------------------------------------------------
# BUILDROOT VERSION
# -----------------------------------------------------------------------------
# We pin a specific version for reproducibility. Upgrading Buildroot can
# change behavior, so we do it deliberately, not accidentally.

BUILDROOT_VERSION := 2025.11.1
BUILDROOT_TARBALL := buildroot-$(BUILDROOT_VERSION).tar.gz
BUILDROOT_URL := https://buildroot.org/downloads/$(BUILDROOT_TARBALL)
# Checksum available at https://buildroot.org/downloads/ (not currently verified)
BUILDROOT_SHA256 := none

# -----------------------------------------------------------------------------
# DIRECTORY LAYOUT
# -----------------------------------------------------------------------------
# Understanding where things go:
#
#   platform/buildroot/
#   |-- Makefile          <- You are here
#   |-- upstream/         <- Extracted Buildroot source
#   |-- configs/          <- Our defconfig files
#   |-- overlay/          <- Files to add to rootfs
#   |-- dl/               <- Downloaded tarballs (cached)
#   |-- output/
#   |   |-- x86_64/       <- x86_64 build output
#   |   +-- aarch64/      <- aarch64 build output
#   +-- .stamps/          <- Build progress markers

MAKEFILE_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
TOP_DIR := $(realpath $(MAKEFILE_DIR)/../..)
BR_DIR := $(MAKEFILE_DIR)/upstream
OUTPUT_X86 := $(MAKEFILE_DIR)/output/x86_64
OUTPUT_ARM := $(MAKEFILE_DIR)/output/aarch64
DL_DIR := $(MAKEFILE_DIR)/dl
CONFIG_DIR := $(MAKEFILE_DIR)/configs
OVERLAY_DIR := $(MAKEFILE_DIR)/overlay
IMAGES_DIR := $(TOP_DIR)/platform/images

# Our defconfig names - these files live in configs/
DEFCONFIG_X86 := phantomfpga_x86_64_defconfig
DEFCONFIG_ARM := phantomfpga_aarch64_defconfig

# -----------------------------------------------------------------------------
# BUILD PARALLELISM
# -----------------------------------------------------------------------------
# Use all CPU cores by default. Override with: make NPROC=4
# More cores = faster builds, but also more RAM usage.

NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# -----------------------------------------------------------------------------
# STAMP FILES
# -----------------------------------------------------------------------------
# These empty files mark completed build stages. The file's existence (not
# content) indicates the step is done. Delete a stamp to force re-running.
#
# This is a common pattern in Makefiles - much simpler than tracking all
# the actual output files, which can number in the thousands.

STAMP_DIR := $(MAKEFILE_DIR)/.stamps
STAMP_DOWNLOAD := $(STAMP_DIR)/download
STAMP_EXTRACT := $(STAMP_DIR)/extract
STAMP_CONFIG_X86 := $(STAMP_DIR)/config-x86_64
STAMP_CONFIG_ARM := $(STAMP_DIR)/config-aarch64
STAMP_BUILD_X86 := $(STAMP_DIR)/build-x86_64
STAMP_BUILD_ARM := $(STAMP_DIR)/build-aarch64

# Final output files
KERNEL_X86 := bzImage
KERNEL_ARM := Image
ROOTFS := rootfs.ext4

# -----------------------------------------------------------------------------
# PRETTY OUTPUT
# -----------------------------------------------------------------------------
# Colors make build output easier to scan. Disabled when not running in
# a terminal (e.g., in CI logs).

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

# -----------------------------------------------------------------------------
# VERBOSE MODE
# -----------------------------------------------------------------------------
# Set V=1 to see all commands: make V=1
# By default, we hide command output and show progress messages.

V ?= 0
ifeq ($(V),1)
  Q :=
  QUIET :=
else
  Q := @
  QUIET := --quiet
endif

# -----------------------------------------------------------------------------
# PATH SANITIZATION
# -----------------------------------------------------------------------------
# Buildroot refuses to build if PATH contains spaces, tabs, or newlines.
# On WSL, Windows paths like /mnt/c/Program Files/... leak into Linux PATH
# and trigger this check. We strip those entries automatically so "make"
# just works without manual PATH surgery.

_CLEAN_PATH := $(shell echo "$$PATH" | tr ':' '\n' | grep -v '[[:space:]]' | paste -sd ':')
ifneq ($(PATH),$(_CLEAN_PATH))
$(info NOTE: stripped PATH entries containing whitespace (Buildroot needs a clean PATH))
export PATH := $(_CLEAN_PATH)
endif

# =============================================================================
# BUILD TARGETS
# =============================================================================
# The main targets you'll use. Each depends on its stamp file, which
# depends on previous stages.

# Detect host architecture (used for auto-detection and toolchain selection)
HOST_ARCH := $(shell uname -m)

# Auto-detect target architecture based on host
# x86_64 host -> x86_64 target, aarch64/arm64 host -> aarch64 target
ifeq ($(HOST_ARCH),aarch64)
  DEFAULT_TARGET := aarch64
else ifeq ($(HOST_ARCH),arm64)
  DEFAULT_TARGET := aarch64
else
  DEFAULT_TARGET := x86_64
endif

.PHONY: all
all: $(DEFAULT_TARGET)
	@echo ""
	@echo "$(GREEN)Tip:$(NC) Run './platform/run_qemu.sh' to boot the VM."

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

# =============================================================================
# STAGE 1: DOWNLOAD
# =============================================================================
# Download Buildroot source tarball. Cached in dl/ for subsequent builds.
# We try wget first, fall back to curl (macOS doesn't have wget by default).

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

# =============================================================================
# STAGE 2: EXTRACT
# =============================================================================
# Unpack Buildroot source. We delete any existing extraction first to ensure
# a clean state (important if switching versions).

$(STAMP_EXTRACT): $(STAMP_DOWNLOAD)
	@echo "$(BLUE)==> Extracting Buildroot...$(NC)"
	$(Q)rm -rf "$(BR_DIR)"
	$(Q)mkdir -p "$(BR_DIR)"
	$(Q)tar -xzf "$(DL_DIR)/$(BUILDROOT_TARBALL)" -C "$(BR_DIR)" --strip-components=1
	@touch $@

# =============================================================================
# STAGE 3: CONFIGURE
# =============================================================================
# Apply our defconfig to create a full .config. This sets up:
#   - Target architecture (x86_64 or aarch64)
#   - Kernel version and config
#   - Packages to include (busybox, dropbear, etc.)
#   - Rootfs format (ext4)
#
# TOOLCHAIN SELECTION (automatic):
#   - x86_64 host: Use Bootlin pre-built toolchain (fast download)
#   - aarch64 host: Build toolchain from source (slow, ~15 min)
#
# Note: Distro cross-compilers (apt install gcc-x86-64-linux-gnu) don't work
# with Buildroot as they're non-relocatable. On non-x86 hosts, Buildroot
# must build its own cross-compiler.

# HOST_ARCH is defined earlier (before BUILD TARGETS) for auto-detection

# Toolchain fragments for Bootlin (x86_64 hosts only)
TC_BOOTLIN_X86 := $(CONFIG_DIR)/toolchain-bootlin-x86_64.fragment
TC_BOOTLIN_ARM := $(CONFIG_DIR)/toolchain-bootlin-aarch64.fragment

.PHONY: configure-x86_64
configure-x86_64: $(STAMP_CONFIG_X86)

$(STAMP_CONFIG_X86): $(STAMP_EXTRACT) | $(OUTPUT_X86)
	@echo "$(BLUE)==> Configuring x86_64 build...$(NC)"
	$(Q)cp "$(CONFIG_DIR)/$(DEFCONFIG_X86)" "$(BR_DIR)/configs/"
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" $(DEFCONFIG_X86) \
		BR2_EXTERNAL= \
		BR2_DL_DIR="$(DL_DIR)" \
		$(if $(filter 0,$(V)),>/dev/null 2>&1,)
	@# Append overlay path
	$(Q)echo 'BR2_ROOTFS_OVERLAY="$(OVERLAY_DIR)"' >> "$(OUTPUT_X86)/.config"
	@# Toolchain selection based on host architecture
ifeq ($(HOST_ARCH),x86_64)
	@echo "    $(GREEN)Using Bootlin toolchain (x86_64 host)$(NC)"
	$(Q)cat "$(TC_BOOTLIN_X86)" >> "$(OUTPUT_X86)/.config"
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" olddefconfig \
		$(if $(filter 0,$(V)),>/dev/null 2>&1,)
else
	@echo "    $(YELLOW)Building toolchain from source ($(HOST_ARCH) host)$(NC)"
	@echo "    $(YELLOW)This takes ~15 min. On x86_64 Linux, Bootlin is used instead.$(NC)"
endif
	@touch $@

.PHONY: configure-aarch64
configure-aarch64: $(STAMP_CONFIG_ARM)

$(STAMP_CONFIG_ARM): $(STAMP_EXTRACT) | $(OUTPUT_ARM)
	@echo "$(BLUE)==> Configuring aarch64 build...$(NC)"
	$(Q)cp "$(CONFIG_DIR)/$(DEFCONFIG_ARM)" "$(BR_DIR)/configs/"
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" $(DEFCONFIG_ARM) \
		BR2_EXTERNAL= \
		BR2_DL_DIR="$(DL_DIR)" \
		$(if $(filter 0,$(V)),>/dev/null 2>&1,)
	@# Append overlay path
	$(Q)echo 'BR2_ROOTFS_OVERLAY="$(OVERLAY_DIR)"' >> "$(OUTPUT_ARM)/.config"
	@# Toolchain selection based on host architecture
ifeq ($(HOST_ARCH),x86_64)
	@echo "    $(GREEN)Using Bootlin toolchain (x86_64 host)$(NC)"
	$(Q)cat "$(TC_BOOTLIN_ARM)" >> "$(OUTPUT_ARM)/.config"
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" olddefconfig \
		$(if $(filter 0,$(V)),>/dev/null 2>&1,)
else
	@echo "    $(YELLOW)Building toolchain from source ($(HOST_ARCH) host)$(NC)"
	@echo "    $(YELLOW)This takes ~15 min. On x86_64 Linux, Bootlin is used instead.$(NC)"
endif
	@touch $@

# =============================================================================
# STAGE 4: BUILD
# =============================================================================
# The big one. This compiles:
#   1. Cross-compiler toolchain (takes longest on first build)
#   2. Linux kernel
#   3. All selected packages
#   4. Root filesystem image
#
# First build: 30-60 minutes. Incremental: much faster.
# Output goes to output/<arch>/images/

.PHONY: build-x86_64
build-x86_64: $(STAMP_BUILD_X86)

$(STAMP_BUILD_X86): $(STAMP_CONFIG_X86) | $(IMAGES_DIR)
	@echo "$(BLUE)==> Building x86_64 image (this will take a while)...$(NC)"
	@echo "    Using $(NPROC) parallel jobs"
ifeq ($(V),0)
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" -j$(NPROC) BR2_DL_DIR="$(DL_DIR)"
else
	$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_X86)" -j$(NPROC) BR2_DL_DIR="$(DL_DIR)"
endif
	@echo "$(BLUE)==> Installing x86_64 images...$(NC)"
	$(Q)cp "$(OUTPUT_X86)/images/$(KERNEL_X86)" "$(IMAGES_DIR)/"
	$(Q)cp "$(OUTPUT_X86)/images/$(ROOTFS)" "$(IMAGES_DIR)/"
	@touch $@

.PHONY: build-aarch64
build-aarch64: $(STAMP_BUILD_ARM)

$(STAMP_BUILD_ARM): $(STAMP_CONFIG_ARM) | $(IMAGES_DIR)
	@echo "$(BLUE)==> Building aarch64 image (this will take a while)...$(NC)"
	@echo "    Using $(NPROC) parallel jobs"
ifeq ($(V),0)
	$(Q)$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" -j$(NPROC) BR2_DL_DIR="$(DL_DIR)"
else
	$(MAKE) -C "$(BR_DIR)" O="$(OUTPUT_ARM)" -j$(NPROC) BR2_DL_DIR="$(DL_DIR)"
endif
	@echo "$(BLUE)==> Installing aarch64 images...$(NC)"
	$(Q)cp "$(OUTPUT_ARM)/images/$(KERNEL_ARM)" "$(IMAGES_DIR)/"
	$(Q)cp "$(OUTPUT_ARM)/images/$(ROOTFS)" "$(IMAGES_DIR)/rootfs-aarch64.ext4"
	@touch $@

# =============================================================================
# CONVENIENCE ALIASES
# =============================================================================

.PHONY: configure
configure: configure-x86_64

.PHONY: build
build: build-x86_64

# =============================================================================
# INTERACTIVE CONFIGURATION
# =============================================================================
# menuconfig opens a curses-based UI for editing Buildroot/kernel settings.
# This is how you add packages, change kernel options, etc.
#
# After making changes:
#   1. Save and exit menuconfig
#   2. Run "make savedefconfig" to update our defconfig file
#   3. Commit the updated defconfig

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

# Linux kernel menuconfig - for kernel-specific options like drivers,
# filesystems, etc.
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

# =============================================================================
# SAVING CONFIGURATION CHANGES
# =============================================================================
# After editing with menuconfig, save changes to our defconfig file.
# Defconfig only stores non-default values, keeping it minimal.

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

# =============================================================================
# CLEANING UP
# =============================================================================
# Different levels of cleaning:
#   clean       - Remove build output, keep downloads
#   clean-*     - Clean specific architecture
#   clean-images- Just remove final images
#   distclean   - Remove EVERYTHING, start from scratch

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

# =============================================================================
# STATUS AND HELP
# =============================================================================

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
	@echo "  make              Build image for your architecture (auto-detected: $(DEFAULT_TARGET))"
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
	@echo ""
	@echo "$(YELLOW)Want to learn more?$(NC) Read this Makefile! It's heavily commented."

# =============================================================================
# DIRECTORY CREATION
# =============================================================================
# Order-only prerequisites (after |) - only check existence, not timestamp.

$(DL_DIR) $(OUTPUT_X86) $(OUTPUT_ARM) $(STAMP_DIR) $(IMAGES_DIR):
	$(Q)mkdir -p $@

# =============================================================================
# PARALLEL BUILD CONTROL
# =============================================================================
# .NOTPARALLEL prevents Make from running our top-level targets in parallel.
# Buildroot handles its own parallelism with -j$(NPROC).

.NOTPARALLEL:

# Declare all phony targets
.PHONY: all x86_64 aarch64 all-arches download configure configure-x86_64 \
        configure-aarch64 build build-x86_64 build-aarch64 menuconfig \
        menuconfig-x86_64 menuconfig-aarch64 linux-menuconfig \
        linux-menuconfig-x86_64 linux-menuconfig-aarch64 savedefconfig \
        savedefconfig-x86_64 savedefconfig-aarch64 clean clean-x86_64 \
        clean-aarch64 clean-images distclean info help
