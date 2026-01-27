#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# PhantomFPGA Buildroot Build Script for CI
#
# Builds Buildroot guest images for CI environments.
# Supports caching of downloads and incremental builds.
#
# Usage:
#   ./build-buildroot.sh [OPTIONS]
#
# Options:
#   --arch ARCH       Target architecture (x86_64, aarch64, or both)
#   --jobs N          Number of parallel jobs (default: nproc)
#   --output DIR      Output directory for images
#   --download-dir    Shared download directory (for caching)
#   --help            Show help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Defaults
ARCH="x86_64"
JOBS=$(nproc 2>/dev/null || echo 4)
OUTPUT_DIR=""
DOWNLOAD_DIR=""

# Logging
info() { echo "[BUILDROOT] $*"; }
error() { echo "[BUILDROOT] ERROR: $*" >&2; }
die() { error "$*"; exit 1; }

usage() {
    cat << EOF
PhantomFPGA Buildroot Build Script

Usage: $(basename "$0") [OPTIONS]

Options:
    --arch ARCH         Target: x86_64, aarch64, or both (default: x86_64)
    --jobs N            Parallel jobs (default: $JOBS)
    --output DIR        Output directory for images
    --download-dir DIR  Shared download directory for caching
    --help              Show this help

Examples:
    $(basename "$0")                        # Build x86_64
    $(basename "$0") --arch both            # Build both architectures
    $(basename "$0") --download-dir /cache  # Use shared download cache
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
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --download-dir)
            DOWNLOAD_DIR="$2"
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

# Install dependencies
install_dependencies() {
    info "Checking build dependencies..."

    local deps_needed=false

    for cmd in wget make gcc cpio rsync bc; do
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
                build-essential \
                wget \
                cpio \
                unzip \
                rsync \
                bc \
                libncurses-dev \
                file \
                locales

            # Ensure en_US.UTF-8 locale
            sudo locale-gen en_US.UTF-8 || true
        elif [ -f /etc/redhat-release ]; then
            sudo dnf install -y \
                gcc \
                make \
                wget \
                cpio \
                unzip \
                rsync \
                bc \
                ncurses-devel \
                file
        else
            error "Unsupported distribution. Please install dependencies manually."
            return 1
        fi
    fi

    info "Dependencies OK"
}

# Build Buildroot images
build_buildroot() {
    cd "${PROJECT_ROOT}/platform/buildroot"

    # Set up download directory if specified
    local make_args="NPROC=$JOBS V=0"

    if [ -n "$DOWNLOAD_DIR" ]; then
        info "Using shared download directory: $DOWNLOAD_DIR"
        mkdir -p "$DOWNLOAD_DIR"

        # Link or set the download directory
        if [ ! -e dl ]; then
            ln -s "$DOWNLOAD_DIR" dl
        fi
    fi

    case "$ARCH" in
        x86_64)
            info "Building x86_64 image..."
            make x86_64 $make_args
            ;;
        aarch64)
            info "Building aarch64 image..."
            make aarch64 $make_args
            ;;
        both)
            info "Building x86_64 image..."
            make x86_64 $make_args
            info "Building aarch64 image..."
            make aarch64 $make_args
            ;;
    esac

    info "Build complete!"
}

# Verify build
verify_build() {
    info "Verifying build outputs..."

    local images_dir="${PROJECT_ROOT}/platform/images"

    if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "both" ]; then
        if [ -f "${images_dir}/bzImage" ] && [ -f "${images_dir}/rootfs.ext4" ]; then
            info "x86_64: Kernel and rootfs found"
            ls -lh "${images_dir}/bzImage" "${images_dir}/rootfs.ext4"
        else
            die "x86_64: Missing kernel or rootfs!"
        fi
    fi

    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "both" ]; then
        if [ -f "${images_dir}/Image" ] && [ -f "${images_dir}/rootfs-aarch64.ext4" ]; then
            info "aarch64: Kernel and rootfs found"
            ls -lh "${images_dir}/Image" "${images_dir}/rootfs-aarch64.ext4"
        else
            die "aarch64: Missing kernel or rootfs!"
        fi
    fi

    info "Verification passed"
}

# Copy outputs
copy_outputs() {
    if [ -z "$OUTPUT_DIR" ]; then
        return 0
    fi

    info "Copying images to $OUTPUT_DIR..."
    mkdir -p "$OUTPUT_DIR"

    local images_dir="${PROJECT_ROOT}/platform/images"

    if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "both" ]; then
        cp "${images_dir}/bzImage" "$OUTPUT_DIR/"
        cp "${images_dir}/rootfs.ext4" "$OUTPUT_DIR/"
    fi

    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "both" ]; then
        cp "${images_dir}/Image" "$OUTPUT_DIR/"
        cp "${images_dir}/rootfs-aarch64.ext4" "$OUTPUT_DIR/"
    fi

    info "Images copied to $OUTPUT_DIR"
    ls -la "$OUTPUT_DIR"
}

# Main
main() {
    local start_time=$(date +%s)

    info "PhantomFPGA Buildroot Build"
    info "==========================="
    info "Architecture: $ARCH"
    info "Parallel jobs: $JOBS"
    info ""

    install_dependencies
    build_buildroot
    verify_build
    copy_outputs

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    info "Build time: $((duration / 60))m $((duration % 60))s"
    info "=== BUILD SUCCESSFUL ==="
}

main "$@"
