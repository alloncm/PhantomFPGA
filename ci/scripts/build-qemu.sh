#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# PhantomFPGA QEMU Build Script for CI
#
# Builds QEMU with the PhantomFPGA device for CI environments.
# Optimized for caching and minimal build times.
#
# Usage:
#   ./build-qemu.sh [OPTIONS]
#
# Options:
#   --arch ARCH       Target architecture (x86_64, aarch64, or both)
#   --shallow         Shallow clone QEMU (faster)
#   --jobs N          Number of parallel jobs (default: nproc)
#   --output DIR      Output directory for binaries
#   --help            Show help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Defaults
ARCH="x86_64"
SHALLOW=true
JOBS=$(nproc 2>/dev/null || echo 4)
OUTPUT_DIR=""

# Logging
info() { echo "[BUILD] $*"; }
error() { echo "[BUILD] ERROR: $*" >&2; }
die() { error "$*"; exit 1; }

usage() {
    cat << EOF
PhantomFPGA QEMU Build Script

Usage: $(basename "$0") [OPTIONS]

Options:
    --arch ARCH       Target: x86_64, aarch64, or both (default: x86_64)
    --shallow         Shallow clone QEMU repo (default: true)
    --no-shallow      Full clone QEMU repo
    --jobs N          Parallel jobs (default: $JOBS)
    --output DIR      Output directory for binaries
    --help            Show this help

Examples:
    $(basename "$0")                    # Build x86_64
    $(basename "$0") --arch both        # Build both architectures
    $(basename "$0") --jobs 8           # Use 8 parallel jobs
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
        --shallow)
            SHALLOW=true
            shift
            ;;
        --no-shallow)
            SHALLOW=false
            shift
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --help|-h)
            usage
            ;;
        *)
            die "Unknown option: $1"
            ;;
    esac
done

# Validate architecture
case "$ARCH" in
    x86_64|aarch64|both)
        ;;
    *)
        die "Invalid architecture: $ARCH"
        ;;
esac

# Install dependencies (if running as root or with sudo)
install_dependencies() {
    info "Checking build dependencies..."

    local deps_needed=false

    for cmd in git ninja meson pkg-config gcc; do
        if ! command -v "$cmd" &>/dev/null; then
            deps_needed=true
            break
        fi
    done

    if [ "$deps_needed" = true ]; then
        info "Installing build dependencies..."

        if [ -f /etc/debian_version ]; then
            sudo apt-get update
            sudo apt-get install -y --no-install-recommends \
                git \
                build-essential \
                ninja-build \
                python3 \
                python3-pip \
                pkg-config \
                libglib2.0-dev \
                libpixman-1-dev \
                libslirp-dev \
                libcap-ng-dev \
                libattr1-dev \
                meson
        elif [ -f /etc/redhat-release ]; then
            sudo dnf install -y \
                git \
                gcc \
                ninja-build \
                python3 \
                python3-pip \
                pkg-config \
                glib2-devel \
                pixman-devel \
                libslirp-devel \
                meson
        else
            error "Unsupported distribution. Please install dependencies manually."
            return 1
        fi
    fi

    info "Dependencies OK"
}

# Build QEMU
build_qemu() {
    local setup_args=""

    if [ "$SHALLOW" = true ]; then
        setup_args="--shallow"
    fi

    info "Running QEMU setup..."
    cd "${PROJECT_ROOT}/platform/qemu"
    ./setup.sh $setup_args

    info "Building QEMU with $JOBS parallel jobs..."
    cd build

    case "$ARCH" in
        x86_64)
            ninja -j"$JOBS" qemu-system-x86_64
            ;;
        aarch64)
            ninja -j"$JOBS" qemu-system-aarch64
            ;;
        both)
            ninja -j"$JOBS" qemu-system-x86_64 qemu-system-aarch64
            ;;
    esac

    info "Build complete!"
}

# Verify build
verify_build() {
    info "Verifying PhantomFPGA device..."

    local build_dir="${PROJECT_ROOT}/platform/qemu/build"

    if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "both" ]; then
        if "${build_dir}/qemu-system-x86_64" -device help 2>&1 | grep -q phantomfpga; then
            info "x86_64: PhantomFPGA device found"
        else
            die "x86_64: PhantomFPGA device NOT found!"
        fi
    fi

    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "both" ]; then
        if "${build_dir}/qemu-system-aarch64" -device help 2>&1 | grep -q phantomfpga; then
            info "aarch64: PhantomFPGA device found"
        else
            die "aarch64: PhantomFPGA device NOT found!"
        fi
    fi

    info "Verification passed"
}

# Copy outputs
copy_outputs() {
    if [ -z "$OUTPUT_DIR" ]; then
        return 0
    fi

    info "Copying binaries to $OUTPUT_DIR..."
    mkdir -p "$OUTPUT_DIR"

    local build_dir="${PROJECT_ROOT}/platform/qemu/build"

    if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "both" ]; then
        cp "${build_dir}/qemu-system-x86_64" "$OUTPUT_DIR/"
    fi

    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "both" ]; then
        cp "${build_dir}/qemu-system-aarch64" "$OUTPUT_DIR/"
    fi

    info "Binaries copied to $OUTPUT_DIR"
    ls -la "$OUTPUT_DIR"
}

# Main
main() {
    info "PhantomFPGA QEMU Build"
    info "======================"
    info "Architecture: $ARCH"
    info "Parallel jobs: $JOBS"
    info "Shallow clone: $SHALLOW"
    info ""

    install_dependencies
    build_qemu
    verify_build
    copy_outputs

    info "=== BUILD SUCCESSFUL ==="
}

main "$@"
