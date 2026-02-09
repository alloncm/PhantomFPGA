# PhantomFPGA QEMU Makefile
#
# Builds QEMU with the PhantomFPGA virtual device integrated.
# The build process:
#   1. Clone/update QEMU submodule
#   2. Copy our device files into the QEMU tree
#   3. Apply patches to integrate with QEMU build system
#   4. Configure and build QEMU
#
# "Why build QEMU from source?" you ask. Because we're adding a device
# that doesn't exist upstream. It's like asking why you'd modify your car
# when you could just buy a new one. Some of us enjoy the pain.

# -----------------------------------------------------------------------------
# Configuration Variables
# -----------------------------------------------------------------------------

QEMU_VERSION ?= v10.2.0
QEMU_DIR := $(CURDIR)/upstream
QEMU_BUILD := $(CURDIR)/build

# Target architecture: x86_64 or aarch64 (default: x86_64)
TARGET_ARCH ?= x86_64
QEMU_BINARY := $(QEMU_BUILD)/qemu-system-$(TARGET_ARCH)

# Build both architectures
QEMU_TARGETS := x86_64-softmmu,aarch64-softmmu

# Build parallelism
NPROC := $(shell nproc 2>/dev/null || echo 4)

# Our device source files
SRC_DIR := $(CURDIR)/src/hw/misc
PATCH_DIR := $(CURDIR)/patches

# Where to copy our files in QEMU tree
QEMU_MISC_DIR := $(QEMU_DIR)/hw/misc

# Stamp files to track what's been done
STAMP_DIR := $(CURDIR)/.stamps
STAMP_CLONE := $(STAMP_DIR)/clone
STAMP_PATCH := $(STAMP_DIR)/patch
STAMP_COPY := $(STAMP_DIR)/copy
STAMP_CONFIGURE := $(STAMP_DIR)/configure
STAMP_BUILD := $(STAMP_DIR)/build

# Configure options - build both x86_64 and aarch64 targets
CONFIGURE_OPTS := \
	--target-list=$(QEMU_TARGETS) \
	--enable-kvm \
	--enable-virtfs \
	--enable-slirp \
	--enable-debug \
	--disable-docs

# Device source files
DEVICE_SOURCES := $(SRC_DIR)/phantomfpga.c $(SRC_DIR)/phantomfpga.h $(SRC_DIR)/frames_data.h

# -----------------------------------------------------------------------------
# Phony Targets
# -----------------------------------------------------------------------------

.PHONY: all setup clean distclean help patch copy configure build rebuild
.PHONY: sync reconfigure check info run list-devices

# Default: show help
.DEFAULT_GOAL := help

all: build

# -----------------------------------------------------------------------------
# Help
# -----------------------------------------------------------------------------

help:
	@echo ""
	@echo "PhantomFPGA QEMU Build System"
	@echo "============================="
	@echo ""
	@echo "Main Targets:"
	@echo "  make setup      - Run setup.sh (clone, patch, configure)"
	@echo "  make build      - Build QEMU (runs setup if needed)"
	@echo "  make rebuild    - Quick rebuild after source changes"
	@echo "  make all        - Alias for 'build'"
	@echo ""
	@echo "Step-by-Step Targets:"
	@echo "  make clone      - Clone QEMU repository"
	@echo "  make copy       - Copy device files to QEMU tree"
	@echo "  make patch      - Apply integration patches"
	@echo "  make configure  - Configure QEMU build"
	@echo ""
	@echo "Utility Targets:"
	@echo "  make sync       - Copy device sources (for incremental dev)"
	@echo "  make reconfigure- Re-run configure"
	@echo "  make check      - Check build state"
	@echo "  make run        - Quick test run with device"
	@echo "  make list-devices - List QEMU devices matching 'phantom'"
	@echo ""
	@echo "Cleanup Targets:"
	@echo "  make clean      - Remove build artifacts"
	@echo "  make distclean  - Remove everything including QEMU clone"
	@echo ""
	@echo "Variables:"
	@echo "  QEMU_VERSION - QEMU version to build (default: $(QEMU_VERSION))"
	@echo "  TARGET_ARCH  - Target architecture for 'run' (default: x86_64)"
	@echo "  NPROC        - Build parallelism (default: $(NPROC))"
	@echo ""
	@echo "Supported Architectures:"
	@echo "  x86_64  - Intel/AMD 64-bit (default)"
	@echo "  aarch64 - ARM 64-bit"
	@echo ""
	@echo "Output Binaries:"
	@echo "  $(QEMU_BUILD)/qemu-system-x86_64"
	@echo "  $(QEMU_BUILD)/qemu-system-aarch64"
	@echo ""

# -----------------------------------------------------------------------------
# Setup via script (recommended method)
# -----------------------------------------------------------------------------

setup: $(CURDIR)/setup.sh $(DEVICE_SOURCES)
	@chmod +x $(CURDIR)/setup.sh
	$(CURDIR)/setup.sh
	@mkdir -p $(STAMP_DIR)
	@touch $(STAMP_CLONE) $(STAMP_PATCH) $(STAMP_COPY) $(STAMP_CONFIGURE)

# -----------------------------------------------------------------------------
# Step-by-Step Targets (alternative to setup.sh)
# -----------------------------------------------------------------------------

$(STAMP_DIR):
	mkdir -p $(STAMP_DIR)

# Clone QEMU repository
clone: $(STAMP_CLONE)
$(STAMP_CLONE): | $(STAMP_DIR)
	@echo "=== Cloning QEMU $(QEMU_VERSION) ==="
	@if [ ! -d "$(QEMU_DIR)" ]; then \
		git clone --depth 1 --branch $(QEMU_VERSION) \
			https://gitlab.com/qemu-project/qemu.git $(QEMU_DIR); \
	else \
		echo "QEMU directory exists, skipping clone"; \
	fi
	touch $@

# Copy our device files into QEMU tree
# This must happen BEFORE patching so the build system can find them
copy: $(STAMP_COPY)
$(STAMP_COPY): $(STAMP_CLONE) $(DEVICE_SOURCES) | $(STAMP_DIR)
	@echo "=== Copying PhantomFPGA device files ==="
	cp $(SRC_DIR)/phantomfpga.c $(QEMU_MISC_DIR)/
	cp $(SRC_DIR)/phantomfpga.h $(QEMU_MISC_DIR)/
	cp $(SRC_DIR)/frames_data.h $(QEMU_MISC_DIR)/
	@# Update meson.build if needed
	@if ! grep -q "phantomfpga" "$(QEMU_MISC_DIR)/meson.build" 2>/dev/null; then \
		echo "" >> "$(QEMU_MISC_DIR)/meson.build"; \
		echo "# PhantomFPGA device" >> "$(QEMU_MISC_DIR)/meson.build"; \
		echo "system_ss.add(when: 'CONFIG_PHANTOMFPGA', if_true: files('phantomfpga.c'))" >> "$(QEMU_MISC_DIR)/meson.build"; \
		echo "Updated meson.build"; \
	fi
	@# Update Kconfig if needed
	@if ! grep -q "PHANTOMFPGA" "$(QEMU_MISC_DIR)/Kconfig" 2>/dev/null; then \
		echo "" >> "$(QEMU_MISC_DIR)/Kconfig"; \
		echo "config PHANTOMFPGA" >> "$(QEMU_MISC_DIR)/Kconfig"; \
		echo "    bool" >> "$(QEMU_MISC_DIR)/Kconfig"; \
		echo "    default y" >> "$(QEMU_MISC_DIR)/Kconfig"; \
		echo "    depends on PCI" >> "$(QEMU_MISC_DIR)/Kconfig"; \
		echo "Updated Kconfig"; \
	fi
	touch $@

# Apply patches to integrate with QEMU build system
patch: $(STAMP_PATCH)
$(STAMP_PATCH): $(STAMP_COPY) | $(STAMP_DIR)
	@echo "=== Applying PhantomFPGA patches ==="
	@for p in $(PATCH_DIR)/*.patch; do \
		if [ -f "$$p" ]; then \
			echo "Applying: $$p"; \
			(cd $(QEMU_DIR) && git apply --check "$$p" 2>/dev/null && git apply "$$p") || \
			(echo "Patch already applied or N/A: $$p"; true); \
		fi; \
	done
	touch $@

# Configure QEMU
# We build a minimal x86_64 system emulator - just enough for our testing
configure: $(STAMP_CONFIGURE)
$(STAMP_CONFIGURE): $(STAMP_PATCH) | $(STAMP_DIR)
	@echo "=== Configuring QEMU ==="
	mkdir -p $(QEMU_BUILD)
	cd $(QEMU_BUILD) && $(QEMU_DIR)/configure $(CONFIGURE_OPTS)
	touch $@

# -----------------------------------------------------------------------------
# Build Targets
# -----------------------------------------------------------------------------

# Build QEMU
build: $(STAMP_BUILD)
$(STAMP_BUILD): $(STAMP_CONFIGURE) | $(STAMP_DIR)
	@echo "=== Building QEMU ==="
	@# Silence the scary "fatal: No names found" message from dtc subproject.
	@# The dtc build uses 'git describe' for versioning, which fails without tags.
	@# This is harmless but looks alarming, so we create a tag to make it happy.
	@if [ -d "$(QEMU_DIR)/subprojects/dtc/.git" ]; then \
		(cd "$(QEMU_DIR)/subprojects/dtc" && git describe --tags >/dev/null 2>&1) || \
		(cd "$(QEMU_DIR)/subprojects/dtc" && git tag -a "v1.6.0" -m "Version for build" HEAD 2>/dev/null) || true; \
	fi
	cd $(QEMU_BUILD) && ninja -j$(NPROC)
	@echo ""
	@echo "Build complete! Binary at: $(QEMU_BINARY)"
	@echo "To verify PhantomFPGA device:"
	@echo "  $(QEMU_BINARY) -device help 2>&1 | grep -i phantom"
	touch $@

# Quick rebuild after device source changes
rebuild: sync
	@echo "=== Rebuilding QEMU ==="
	@if [ ! -f "$(QEMU_BUILD)/build.ninja" ]; then \
		echo "Error: QEMU not configured. Run 'make configure' first."; \
		exit 1; \
	fi
	cd $(QEMU_BUILD) && ninja -j$(NPROC)
	@echo "Rebuild complete!"

# Just sync sources without full rebuild
sync:
	@echo "=== Syncing device sources ==="
	@if [ ! -d "$(QEMU_MISC_DIR)" ]; then \
		echo "Error: QEMU not cloned. Run 'make clone' first."; \
		exit 1; \
	fi
	cp -v $(SRC_DIR)/phantomfpga.c $(QEMU_MISC_DIR)/
	cp -v $(SRC_DIR)/phantomfpga.h $(QEMU_MISC_DIR)/
	cp -v $(SRC_DIR)/frames_data.h $(QEMU_MISC_DIR)/

# Re-run configure (useful after changing options)
reconfigure:
	@echo "=== Reconfiguring QEMU ==="
	rm -f $(STAMP_CONFIGURE)
	$(MAKE) configure

# -----------------------------------------------------------------------------
# Utility Targets
# -----------------------------------------------------------------------------

# Check build state
check:
	@echo ""
	@echo "PhantomFPGA QEMU Build Status"
	@echo "=============================="
	@echo ""
	@echo "QEMU Version:   $(QEMU_VERSION)"
	@echo "QEMU Source:    $(if $(wildcard $(QEMU_DIR)),present,MISSING)"
	@echo "Build Dir:      $(if $(wildcard $(QEMU_BUILD)),present,MISSING)"
	@echo "Configured:     $(if $(wildcard $(QEMU_BUILD)/build.ninja),YES,no)"
	@echo "Binary:         $(if $(wildcard $(QEMU_BINARY)),BUILT,not built)"
	@echo ""
	@echo "Stamps:"
	@echo "  Clone:        $(if $(wildcard $(STAMP_CLONE)),done,pending)"
	@echo "  Copy:         $(if $(wildcard $(STAMP_COPY)),done,pending)"
	@echo "  Patch:        $(if $(wildcard $(STAMP_PATCH)),done,pending)"
	@echo "  Configure:    $(if $(wildcard $(STAMP_CONFIGURE)),done,pending)"
	@echo "  Build:        $(if $(wildcard $(STAMP_BUILD)),done,pending)"
	@echo ""
	@if [ -f "$(QEMU_BINARY)" ]; then \
		echo "QEMU is ready!"; \
		echo "Version: $$($(QEMU_BINARY) --version 2>/dev/null | head -1)"; \
	else \
		echo "Run 'make build' to complete the build."; \
	fi
	@echo ""

# Show build info
info:
	@echo "QEMU_VERSION  = $(QEMU_VERSION)"
	@echo "QEMU_DIR      = $(QEMU_DIR)"
	@echo "QEMU_BUILD    = $(QEMU_BUILD)"
	@echo "QEMU_BINARY   = $(QEMU_BINARY)"
	@echo "SRC_DIR       = $(SRC_DIR)"
	@echo "PATCH_DIR     = $(PATCH_DIR)"
	@echo "CONFIGURE_OPTS= $(CONFIGURE_OPTS)"
	@echo "NPROC         = $(NPROC)"

# Quick test run with our device
run: $(QEMU_BINARY)
	@echo "=== Starting QEMU with PhantomFPGA device ==="
	@echo "(Use Ctrl+A, X to exit)"
	@echo ""
	$(QEMU_BINARY) \
		-machine q35 \
		-cpu host \
		-enable-kvm \
		-m 512M \
		-device phantomfpga \
		-nographic \
		-monitor none \
		-serial stdio

# List available devices matching 'phantom'
list-devices: $(QEMU_BINARY)
	@echo "=== Checking for PhantomFPGA device ==="
	@$(QEMU_BINARY) -device help 2>&1 | grep -i phantom || \
		(echo "PhantomFPGA device not found! Build may have failed." && exit 1)

# -----------------------------------------------------------------------------
# Cleanup
# -----------------------------------------------------------------------------

# Clean build artifacts but keep QEMU clone
clean:
	@echo "=== Cleaning build artifacts ==="
	@if [ -d "$(QEMU_BUILD)" ] && [ -f "$(QEMU_BUILD)/build.ninja" ]; then \
		cd $(QEMU_BUILD) && ninja clean 2>/dev/null || true; \
	fi
	rm -rf $(QEMU_BUILD)
	rm -f $(STAMP_CONFIGURE) $(STAMP_BUILD)
	@echo "Clean complete"

# Remove everything including QEMU clone
distclean: clean
	@echo "=== Removing QEMU clone and all artifacts ==="
	rm -rf $(QEMU_DIR)
	rm -rf $(STAMP_DIR)
	@echo "Distclean complete"
