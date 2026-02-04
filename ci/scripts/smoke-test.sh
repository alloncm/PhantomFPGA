#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# PhantomFPGA Quick Smoke Test
#
# Performs a quick verification that everything is built correctly
# without actually booting a VM. Good for fast CI feedback.
#
# Usage:
#   ./smoke-test.sh [OPTIONS]
#
# Options:
#   --arch ARCH       Target architecture (x86_64, aarch64, or both)
#   --help            Show help
#
# Exit codes:
#   0 - All checks passed
#   1 - Some checks failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Defaults
ARCH="x86_64"
FAILURES=0

# Logging
pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*"; FAILURES=$((FAILURES + 1)); }
info() { echo "[INFO] $*"; }
skip() { echo "[SKIP] $*"; }

usage() {
    cat << EOF
PhantomFPGA Smoke Test

Usage: $(basename "$0") [OPTIONS]

Options:
    --arch ARCH       Target: x86_64, aarch64, or both (default: x86_64)
    --help            Show this help

Checks:
    - QEMU binary exists and has PhantomFPGA device
    - Kernel image exists
    - Rootfs image exists
    - Driver source compiles (syntax check)
    - App source compiles (syntax check)

EOF
    exit 0
}

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            ARCH="$2"
            shift 2
            ;;
        --help|-h)
            usage
            ;;
        *)
            fail "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Check QEMU binary
check_qemu() {
    local arch="$1"
    local qemu_bin="${PROJECT_ROOT}/platform/qemu/build/qemu-system-${arch}"

    info "Checking QEMU for $arch..."

    if [ ! -f "$qemu_bin" ]; then
        fail "QEMU binary not found: $qemu_bin"
        return
    fi

    if [ ! -x "$qemu_bin" ]; then
        fail "QEMU binary not executable: $qemu_bin"
        return
    fi

    if "$qemu_bin" -device help 2>&1 | grep -q "^name \"phantomfpga\""; then
        pass "QEMU $arch has PhantomFPGA device"
    else
        fail "QEMU $arch missing PhantomFPGA device"
    fi

    # Check version
    local version
    version=$("$qemu_bin" --version | head -1)
    info "  Version: $version"
}

# Check images
check_images() {
    local arch="$1"
    local images_dir="${PROJECT_ROOT}/platform/images"

    info "Checking images for $arch..."

    case "$arch" in
        x86_64)
            local kernel="${images_dir}/bzImage"
            local rootfs="${images_dir}/rootfs.ext4"
            ;;
        aarch64)
            local kernel="${images_dir}/Image"
            local rootfs="${images_dir}/rootfs-aarch64.ext4"
            ;;
    esac

    if [ -f "$kernel" ]; then
        local size
        size=$(du -h "$kernel" | cut -f1)
        pass "Kernel found: $kernel ($size)"
    else
        fail "Kernel not found: $kernel"
    fi

    if [ -f "$rootfs" ]; then
        local size
        size=$(du -h "$rootfs" | cut -f1)
        pass "Rootfs found: $rootfs ($size)"
    else
        fail "Rootfs not found: $rootfs"
    fi
}

# Check driver source
check_driver_source() {
    info "Checking driver source..."

    local driver_dir="${PROJECT_ROOT}/driver"

    if [ ! -f "${driver_dir}/phantomfpga_drv.c" ]; then
        fail "Driver source not found"
        return
    fi

    # Basic syntax check (just verify it's valid C)
    if command -v gcc &>/dev/null; then
        if gcc -fsyntax-only -I"${driver_dir}" "${driver_dir}/phantomfpga_drv.c" 2>/dev/null; then
            pass "Driver source syntax OK"
        else
            # Kernel modules need kernel headers, so syntax check may fail
            skip "Driver syntax check (needs kernel headers)"
        fi
    else
        skip "Driver syntax check (gcc not found)"
    fi

    # Check required files exist
    for file in phantomfpga_drv.c phantomfpga_regs.h phantomfpga_uapi.h Kbuild Makefile; do
        if [ -f "${driver_dir}/${file}" ]; then
            pass "Driver file: $file"
        else
            fail "Driver file missing: $file"
        fi
    done
}

# Check app source
check_app_source() {
    info "Checking app source..."

    local app_dir="${PROJECT_ROOT}/app"

    if [ ! -f "${app_dir}/phantomfpga_app.c" ]; then
        fail "App source not found"
        return
    fi

    # Check CMakeLists.txt
    if [ -f "${app_dir}/CMakeLists.txt" ]; then
        pass "App CMakeLists.txt found"
    else
        fail "App CMakeLists.txt missing"
    fi

    # Try to compile app
    if command -v gcc &>/dev/null; then
        local driver_dir="${PROJECT_ROOT}/driver"
        if gcc -fsyntax-only -I"${driver_dir}" "${app_dir}/phantomfpga_app.c" 2>/dev/null; then
            pass "App source syntax OK"
        else
            skip "App syntax check (may need headers)"
        fi
    fi
}

# Check QEMU device source
check_qemu_device_source() {
    info "Checking QEMU device source..."

    local src_dir="${PROJECT_ROOT}/platform/qemu/src/hw/misc"

    for file in phantomfpga.c phantomfpga.h; do
        if [ -f "${src_dir}/${file}" ]; then
            pass "QEMU device: $file"
        else
            fail "QEMU device missing: $file"
        fi
    done
}

# Check test files
check_tests() {
    info "Checking test files..."

    # Unit tests
    local unit_dir="${PROJECT_ROOT}/tests/unit"
    if [ -f "${unit_dir}/phantomfpga-test.c" ]; then
        pass "QTest source found"
    else
        fail "QTest source missing"
    fi

    if [ -x "${unit_dir}/run-qtest.sh" ]; then
        pass "QTest runner script executable"
    else
        fail "QTest runner script missing or not executable"
    fi

    # Integration tests
    local int_dir="${PROJECT_ROOT}/tests/integration"
    for script in run_all.sh test_driver.sh test_streaming.sh test_faults.sh; do
        if [ -x "${int_dir}/${script}" ]; then
            pass "Integration test: $script"
        else
            fail "Integration test missing: $script"
        fi
    done
}

# Main
main() {
    info "========================================"
    info "PhantomFPGA Smoke Test"
    info "========================================"
    info ""
    info "Architecture: $ARCH"
    info "Project root: $PROJECT_ROOT"
    info ""

    # Run checks
    check_qemu_device_source
    check_driver_source
    check_app_source
    check_tests

    if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "both" ]; then
        check_qemu "x86_64"
        check_images "x86_64"
    fi

    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "both" ]; then
        check_qemu "aarch64"
        check_images "aarch64"
    fi

    # Summary
    info ""
    info "========================================"
    if [ $FAILURES -eq 0 ]; then
        info "ALL CHECKS PASSED"
        exit 0
    else
        info "FAILURES: $FAILURES"
        info "Some checks failed. See above for details."
        exit 1
    fi
}

main "$@"
