#!/bin/sh
# PhantomFPGA Training Environment Setup
# Sets up paths and convenience aliases for driver development inside the VM.
# The driver is cross-compiled on the host - this VM is for testing only.

# 9p shared directories (mounted from host)
export PHANTOMFPGA_DRIVER_DIR="/mnt/driver"
export PHANTOMFPGA_APP_DIR="/mnt/app"
export PHANTOMFPGA_SHARE_DIR="/mnt/share"

# Add app directory to PATH if it exists
if [ -d "$PHANTOMFPGA_APP_DIR" ]; then
    export PATH="$PHANTOMFPGA_APP_DIR:$PATH"
fi

# Kernel module path
export PHANTOMFPGA_MODULE="${PHANTOMFPGA_DRIVER_DIR}/phantomfpga.ko"

# Module management
alias pf-load='insmod ${PHANTOMFPGA_MODULE}'
alias pf-unload='rmmod phantomfpga'
alias pf-reload='rmmod phantomfpga 2>/dev/null; insmod ${PHANTOMFPGA_MODULE}'
alias pf-status='lsmod | grep phantomfpga'

# Diagnostics
alias pf-logs='dmesg | grep -i phantom'
alias pf-dev='ls -la /dev/phantom* 2>/dev/null || echo "No /dev/phantom* devices found"'
alias pf-pci='lspci -vvv -d 0dad:f00d'
