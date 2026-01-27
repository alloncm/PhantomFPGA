#!/bin/sh
# PhantomFPGA Training Environment Setup
# This script sets up paths for the driver and test application

# 9p shared directories (mounted from host)
export PHANTOMFPGA_DRIVER_DIR="/mnt/driver"
export PHANTOMFPGA_APP_DIR="/mnt/app"

# Add app directory to PATH if it exists
if [ -d "$PHANTOMFPGA_APP_DIR" ]; then
    export PATH="$PHANTOMFPGA_APP_DIR:$PATH"
fi

# Kernel module path hints
export PHANTOMFPGA_MODULE="${PHANTOMFPGA_DRIVER_DIR}/phantom_fpga.ko"

# Convenience aliases
alias pf-load='insmod ${PHANTOMFPGA_MODULE}'
alias pf-unload='rmmod phantom_fpga'
alias pf-reload='rmmod phantom_fpga 2>/dev/null; insmod ${PHANTOMFPGA_MODULE}'
alias pf-status='lsmod | grep phantom_fpga'
alias pf-logs='dmesg | grep -i phantom'
alias pf-dev='ls -la /dev/phantom*'
alias pf-pci='lspci -vvv -d 1234:cafe'

# Build helpers
alias pf-build='make -C ${PHANTOMFPGA_DRIVER_DIR}'
alias pf-clean='make -C ${PHANTOMFPGA_DRIVER_DIR} clean'

# Quick workflow alias: rebuild, reload, and show logs
alias pf-test='pf-build && pf-reload && sleep 0.5 && pf-logs | tail -20'

# Show help on first login
if [ -z "$PHANTOMFPGA_HELP_SHOWN" ]; then
    export PHANTOMFPGA_HELP_SHOWN=1
    echo ""
    echo "PhantomFPGA driver development commands:"
    echo "  pf-load    - Load the kernel module"
    echo "  pf-unload  - Unload the kernel module"
    echo "  pf-reload  - Reload (unload + load)"
    echo "  pf-status  - Show module status"
    echo "  pf-logs    - Show kernel logs for phantom"
    echo "  pf-dev     - List /dev/phantom* devices"
    echo "  pf-pci     - Show PCIe device info"
    echo "  pf-build   - Build the driver"
    echo "  pf-test    - Build, reload, and show logs"
    echo ""
fi
