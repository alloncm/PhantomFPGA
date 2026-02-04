#!/bin/bash
#
# PhantomFPGA QEMU Build Setup
#
# Sets up a custom QEMU build with the PhantomFPGA virtual device.
# This script handles cloning, patching, and configuring QEMU.
#
# "One does not simply walk into QEMU... you configure, patch, and pray."
#
# Usage: ./setup.sh [--clean] [--reconfigure] [--help]
#

set -euo pipefail

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------

QEMU_VERSION="v10.2.0"
QEMU_REPO="https://gitlab.com/qemu-project/qemu.git"

# Paths (relative to this script's directory)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU_DIR="${SCRIPT_DIR}/upstream"
BUILD_DIR="${SCRIPT_DIR}/build"
PATCHES_DIR="${SCRIPT_DIR}/patches"
SRC_DIR="${SCRIPT_DIR}/src"

# QEMU configuration options
# Build both x86_64 and aarch64 targets for cross-platform support
QEMU_TARGETS="x86_64-softmmu,aarch64-softmmu"
QEMU_OPTS=(
    "--target-list=${QEMU_TARGETS}"
    "--enable-slirp"
    "--enable-kvm"
    "--enable-debug"
    "--disable-docs"
    "--enable-virtfs"
)

# Colors for output (if terminal supports them)
if [[ -t 1 ]] && command -v tput &>/dev/null; then
    RED=$(tput setaf 1)
    GREEN=$(tput setaf 2)
    YELLOW=$(tput setaf 3)
    BLUE=$(tput setaf 4)
    RESET=$(tput sgr0)
else
    RED=""
    GREEN=""
    YELLOW=""
    BLUE=""
    RESET=""
fi

# -----------------------------------------------------------------------------
# Utility Functions
# -----------------------------------------------------------------------------

info() {
    echo "${BLUE}[INFO]${RESET} $*"
}

success() {
    echo "${GREEN}[OK]${RESET} $*"
}

warn() {
    echo "${YELLOW}[WARN]${RESET} $*" >&2
}

error() {
    echo "${RED}[ERROR]${RESET} $*" >&2
}

die() {
    error "$@"
    exit 1
}

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Sets up QEMU ${QEMU_VERSION} with the PhantomFPGA device.

Options:
    --clean         Remove QEMU directory and start fresh
    --reconfigure   Re-run configure (useful after option changes)
    --shallow       Shallow clone (faster, but no git history)
    --help          Show this help message

Examples:
    ./setup.sh              # Initial setup
    ./setup.sh --clean      # Clean rebuild from scratch
    ./setup.sh --reconfigure # Just reconfigure, keep source

The script will:
    1. Clone QEMU ${QEMU_VERSION} if not present
    2. Apply patches from patches/ directory
    3. Copy PhantomFPGA device source files
    4. Configure QEMU for building

After running this script, use 'make build' to compile QEMU.
EOF
    exit 0
}

# -----------------------------------------------------------------------------
# Dependency Checks
# -----------------------------------------------------------------------------

check_dependencies() {
    local missing=()

    # Required tools
    for cmd in git ninja python3 pkg-config; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done

    # Check for a C compiler
    if ! command -v gcc &>/dev/null && ! command -v clang &>/dev/null; then
        missing+=("gcc or clang")
    fi

    # Check for meson (QEMU build system)
    if ! command -v meson &>/dev/null; then
        missing+=("meson")
    fi

    # Check for required libraries (best effort via pkg-config)
    if command -v pkg-config &>/dev/null; then
        for lib in glib-2.0 pixman-1; do
            if ! pkg-config --exists "$lib" 2>/dev/null; then
                warn "Library '$lib' not found via pkg-config"
            fi
        done
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        error "Missing required dependencies:"
        for dep in "${missing[@]}"; do
            echo "  - $dep"
        done
        echo ""
        echo "On Debian/Ubuntu, install with:"
        echo "  sudo apt-get install git ninja-build python3 python3-pip pkg-config"
        echo "  sudo apt-get install libglib2.0-dev libpixman-1-dev libslirp-dev"
        echo "  sudo pip3 install meson"
        die "Please install missing dependencies and try again."
    fi

    success "All dependencies found"
}

# -----------------------------------------------------------------------------
# QEMU Clone
# -----------------------------------------------------------------------------

clone_qemu() {
    local shallow_flag=""
    if [[ "${SHALLOW:-false}" == "true" ]]; then
        shallow_flag="--depth=1"
    fi

    if [[ -d "${QEMU_DIR}" ]]; then
        info "QEMU directory exists, checking version..."

        if [[ -d "${QEMU_DIR}/.git" ]]; then
            local current_version
            current_version=$(cd "${QEMU_DIR}" && git describe --tags 2>/dev/null || echo "unknown")

            if [[ "${current_version}" == "${QEMU_VERSION}" ]]; then
                success "QEMU ${QEMU_VERSION} already cloned"
                return 0
            else
                warn "QEMU version mismatch: have ${current_version}, want ${QEMU_VERSION}"
                warn "Use --clean to reset, or manually checkout the correct version"
                # Try to checkout the correct version
                info "Attempting to checkout ${QEMU_VERSION}..."
                (cd "${QEMU_DIR}" && git fetch --tags && git checkout "${QEMU_VERSION}") || \
                    die "Failed to checkout ${QEMU_VERSION}. Use --clean for fresh start."
            fi
        else
            die "QEMU directory exists but is not a git repo. Use --clean."
        fi
    else
        info "Cloning QEMU ${QEMU_VERSION}..."
        git clone ${shallow_flag} --branch "${QEMU_VERSION}" "${QEMU_REPO}" "${QEMU_DIR}" || \
            die "Failed to clone QEMU"
        success "QEMU cloned successfully"
    fi
}

# -----------------------------------------------------------------------------
# Apply Patches
# -----------------------------------------------------------------------------

apply_patches() {
    if [[ ! -d "${PATCHES_DIR}" ]]; then
        info "No patches directory found, skipping"
        return 0
    fi

    # Find all .patch files
    local patches
    patches=$(find "${PATCHES_DIR}" -name "*.patch" -type f 2>/dev/null | sort)

    if [[ -z "${patches}" ]]; then
        info "No patches to apply"
        return 0
    fi

    info "Applying patches..."

    # Create a marker file to track applied patches
    local marker="${QEMU_DIR}/.phantomfpga_patched"

    if [[ -f "${marker}" ]]; then
        info "Patches already applied (found marker)"
        return 0
    fi

    cd "${QEMU_DIR}"

    for patch in ${patches}; do
        local patch_name
        patch_name=$(basename "${patch}")

        info "  Applying: ${patch_name}"

        # Try to apply, allow already-applied patches
        if git apply --check "${patch}" 2>/dev/null; then
            git apply "${patch}" || die "Failed to apply patch: ${patch_name}"
            success "  Applied: ${patch_name}"
        else
            # Check if already applied (reverse applies cleanly)
            if git apply --check --reverse "${patch}" 2>/dev/null; then
                info "  Skipping (already applied): ${patch_name}"
            else
                die "Patch does not apply cleanly: ${patch_name}"
            fi
        fi
    done

    # Create marker
    date > "${marker}"
    success "All patches applied"
}

# -----------------------------------------------------------------------------
# Copy Device Sources
# -----------------------------------------------------------------------------

copy_device_sources() {
    info "Copying PhantomFPGA device sources to QEMU tree..."

    local src_hw="${SRC_DIR}/hw/misc"
    local dst_hw="${QEMU_DIR}/hw/misc"

    if [[ ! -d "${src_hw}" ]]; then
        die "Source directory not found: ${src_hw}"
    fi

    # Copy device files
    for file in phantomfpga.c phantomfpga.h; do
        if [[ -f "${src_hw}/${file}" ]]; then
            cp -v "${src_hw}/${file}" "${dst_hw}/" || die "Failed to copy ${file}"
        else
            die "Missing source file: ${src_hw}/${file}"
        fi
    done

    # Update QEMU build system to include our device
    update_qemu_build_files

    success "Device sources copied"
}

# -----------------------------------------------------------------------------
# Update QEMU Build Configuration
# -----------------------------------------------------------------------------

update_qemu_build_files() {
    info "Updating QEMU build configuration..."

    local meson_file="${QEMU_DIR}/hw/misc/meson.build"
    local kconfig_file="${QEMU_DIR}/hw/misc/Kconfig"

    # Marker to detect if we already modified the files
    local marker_text="# PhantomFPGA device"

    # --- Update meson.build ---
    if grep -q "phantomfpga" "${meson_file}" 2>/dev/null; then
        info "  meson.build already contains phantomfpga"
    else
        info "  Adding phantomfpga to meson.build"

        # Add our device to meson.build
        # We add it as a system-softmmu device that's always built for x86_64
        cat >> "${meson_file}" <<'EOF'

# PhantomFPGA device
system_ss.add(when: 'CONFIG_PHANTOMFPGA', if_true: files('phantomfpga.c'))
EOF
        success "  Updated meson.build"
    fi

    # --- Update Kconfig ---
    if grep -q "PHANTOMFPGA" "${kconfig_file}" 2>/dev/null; then
        info "  Kconfig already contains PHANTOMFPGA"
    else
        info "  Adding PHANTOMFPGA to Kconfig"

        cat >> "${kconfig_file}" <<'EOF'

config PHANTOMFPGA
    bool
    default y
    depends on PCI
EOF
        success "  Updated Kconfig"
    fi

    success "Build configuration updated"
}

# -----------------------------------------------------------------------------
# Configure QEMU
# -----------------------------------------------------------------------------

configure_qemu() {
    info "Configuring QEMU..."

    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"

    # Check if already configured
    if [[ -f "build.ninja" ]] && [[ "${RECONFIGURE:-false}" != "true" ]]; then
        info "QEMU already configured (use --reconfigure to redo)"
        return 0
    fi

    info "Running configure with options:"
    for opt in "${QEMU_OPTS[@]}"; do
        info "  ${opt}"
    done

    "${QEMU_DIR}/configure" "${QEMU_OPTS[@]}" || die "Configure failed"

    success "QEMU configured successfully"
}

# -----------------------------------------------------------------------------
# Clean Up
# -----------------------------------------------------------------------------

do_clean() {
    warn "Removing QEMU directory..."
    rm -rf "${QEMU_DIR}"
    rm -rf "${BUILD_DIR}"
    success "Clean complete"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

main() {
    local clean=false
    local reconfigure=false
    local shallow=false

    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --clean)
                clean=true
                shift
                ;;
            --reconfigure)
                reconfigure=true
                shift
                ;;
            --shallow)
                shallow=true
                shift
                ;;
            --help|-h)
                usage
                ;;
            *)
                die "Unknown option: $1 (use --help for usage)"
                ;;
        esac
    done

    export RECONFIGURE="${reconfigure}"
    export SHALLOW="${shallow}"

    echo ""
    echo "==========================================="
    echo "  PhantomFPGA QEMU Build Setup"
    echo "  QEMU Version: ${QEMU_VERSION}"
    echo "==========================================="
    echo ""

    # Handle clean first
    if [[ "${clean}" == "true" ]]; then
        do_clean
    fi

    # Run setup steps
    check_dependencies
    clone_qemu
    apply_patches
    copy_device_sources
    configure_qemu

    echo ""
    success "Setup complete!"
    echo ""
    echo "Next steps:"
    echo "  1. Build QEMU:     make build"
    echo "  2. Run QEMU:       make run"
    echo ""
    echo "The custom QEMU binaries will be at:"
    echo "  ${BUILD_DIR}/qemu-system-x86_64"
    echo "  ${BUILD_DIR}/qemu-system-aarch64"
    echo ""
}

main "$@"
