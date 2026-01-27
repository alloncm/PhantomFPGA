# SPDX-License-Identifier: GPL-2.0
#
# PhantomFPGA Out-of-Tree Kernel Module Makefile
#
# Usage:
#   make                    - Build against running kernel
#   make KDIR=/path/to/src  - Build against specific kernel
#   make clean              - Clean build artifacts
#   make install            - Install module (requires root)
#   make help               - Show this help
#
# Cross-compilation:
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/kernel
#
# Debug build:
#   make DEBUG=1

# Kernel build directory (default: running kernel)
KDIR ?= /lib/modules/$(shell uname -r)/build

# Current directory (where our source is)
PWD := $(shell pwd)

# Module name
MODULE := phantomfpga

# Enable debug output if requested
ifdef DEBUG
ccflags-y += -DDEBUG -g
endif

# Default target
all: modules

# Build the module
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean build artifacts
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f Module.symvers modules.order

# Install the module
install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# Uninstall the module
uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/$(MODULE).ko
	depmod -a

# Load the module (for testing)
load: modules
	@echo "Loading $(MODULE) module..."
	insmod $(MODULE).ko

# Unload the module
unload:
	@echo "Unloading $(MODULE) module..."
	-rmmod $(MODULE)

# Reload the module
reload: unload load

# Show module info
info: modules
	modinfo $(MODULE).ko

# Check coding style (requires checkpatch.pl from kernel source)
checkpatch:
	@if [ -f $(KDIR)/scripts/checkpatch.pl ]; then \
		$(KDIR)/scripts/checkpatch.pl --no-tree -f *.c *.h; \
	else \
		echo "checkpatch.pl not found in $(KDIR)/scripts/"; \
	fi

# Help target
help:
	@echo "PhantomFPGA Driver Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the kernel module (default)"
	@echo "  clean      - Remove build artifacts"
	@echo "  install    - Install module to system (requires root)"
	@echo "  uninstall  - Remove module from system (requires root)"
	@echo "  load       - Load module with insmod (requires root)"
	@echo "  unload     - Unload module with rmmod (requires root)"
	@echo "  reload     - Unload and reload module"
	@echo "  info       - Show module information"
	@echo "  checkpatch - Check code style"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR       - Kernel source/build directory"
	@echo "               Default: /lib/modules/\$$(uname -r)/build"
	@echo "  DEBUG      - Enable debug build (DEBUG=1)"
	@echo ""
	@echo "Examples:"
	@echo "  make                          # Build for running kernel"
	@echo "  make KDIR=/usr/src/linux-5.15 # Build for specific kernel"
	@echo "  make DEBUG=1                  # Build with debug symbols"
	@echo "  sudo make install             # Install to system"

.PHONY: all modules clean install uninstall load unload reload info checkpatch help
