#!/bin/bash
#
# PhantomFPGA VM Launcher
#
# Launch the QEMU-based training VM with the PhantomFPGA PCIe device.
# Provides various modes for development, debugging, and CI.
#
# "May your registers be responsive and your interrupts timely."
#

set -e

# -----------------------------------------------------------------------------
# Configuration defaults
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Target architecture: x86_64 (default) or aarch64
TARGET_ARCH="x86_64"

# Paths (relative to project root) - will be set based on architecture
QEMU_BUILD="${PROJECT_ROOT}/platform/qemu/build"
DRIVER_DIR="${PROJECT_ROOT}/driver"
APP_DIR="${PROJECT_ROOT}/app"

# These get set after parsing args (depend on TARGET_ARCH)
QEMU_BIN=""
KERNEL_IMAGE=""
ROOTFS_IMAGE=""

# VM configuration defaults
MEMORY="2G"
CPUS="2"
SSH_PORT="2222"
GDB_PORT="1234"

# Mode flags
HEADLESS=0
DEBUG_MODE=0
ENABLE_KVM=1
EXTRA_ARGS=()

# -----------------------------------------------------------------------------
# Help text
# -----------------------------------------------------------------------------
show_help() {
    cat << EOF
PhantomFPGA VM Launcher
=======================

Usage: $(basename "$0") [OPTIONS] [-- EXTRA_QEMU_ARGS...]

Launch the training VM with the PhantomFPGA simulated PCIe device.

OPTIONS:
  -h, --help              Show this help message and exit
  --arch ARCH             Target architecture: x86_64 (default) or aarch64
  --headless              Run without display (for CI/automated testing)
  --debug                 Enable GDB server on port ${GDB_PORT}
  --no-kvm                Disable KVM acceleration (slower, but works anywhere)
  --memory SIZE           Set VM memory (default: ${MEMORY})
  --cpus COUNT            Set number of CPUs (default: ${CPUS})
  --ssh-port PORT         Set SSH port forwarding (default: ${SSH_PORT})

EXAMPLES:
  $(basename "$0")                     # Normal mode with display (x86_64)
  $(basename "$0") --arch aarch64      # Run ARM64 VM
  $(basename "$0") --headless          # Headless for CI (no display)
  $(basename "$0") --debug             # With GDB server on port ${GDB_PORT}
  $(basename "$0") --memory 4G --cpus 4  # Beefier VM
  $(basename "$0") -- -monitor stdio   # Pass extra args to QEMU

GUEST ACCESS:
  SSH:   ssh -p ${SSH_PORT} root@localhost  (password: root)
  GDB:   gdb -ex "target remote :${GDB_PORT}"  (when --debug is used)

SHARED DIRECTORIES:
  The driver/ and app/ directories are mounted in the guest via 9p virtfs:
    mount -t 9p -o trans=virtio driver /mnt/driver
    mount -t 9p -o trans=virtio app /mnt/app

  (These should be auto-mounted at boot via /etc/fstab in the guest)

TROUBLESHOOTING:
  - "Could not access KVM kernel module": Run with --no-kvm or load kvm module
  - Images not found: Run the build first (see platform/buildroot/)
  - QEMU not found: Build QEMU first (see platform/qemu/setup.sh)

EOF
}

# -----------------------------------------------------------------------------
# Logging helpers
# -----------------------------------------------------------------------------
info() {
    echo "[*] $*"
}

error() {
    echo "[ERROR] $*" >&2
}

die() {
    error "$*"
    exit 1
}

warn() {
    echo "[WARN] $*" >&2
}

# -----------------------------------------------------------------------------
# Architecture-specific configuration
# -----------------------------------------------------------------------------
setup_arch_paths() {
    QEMU_BIN="${QEMU_BUILD}/qemu-system-${TARGET_ARCH}"

    case "${TARGET_ARCH}" in
        x86_64)
            KERNEL_IMAGE="${PROJECT_ROOT}/platform/images/bzImage"
            ROOTFS_IMAGE="${PROJECT_ROOT}/platform/images/rootfs.ext4"
            MACHINE_TYPE="q35"
            CPU_TYPE="Nehalem"
            CONSOLE="ttyS0"
            ;;
        aarch64)
            KERNEL_IMAGE="${PROJECT_ROOT}/platform/images/Image"
            ROOTFS_IMAGE="${PROJECT_ROOT}/platform/images/rootfs-aarch64.ext4"
            MACHINE_TYPE="virt"
            CPU_TYPE="cortex-a72"
            CONSOLE="ttyAMA0"
            ;;
    esac
}

# -----------------------------------------------------------------------------
# Validation
# -----------------------------------------------------------------------------
check_prerequisites() {
    local missing=0

    # Check QEMU binary
    if [[ ! -x "${QEMU_BIN}" ]]; then
        error "QEMU binary not found: ${QEMU_BIN}"
        error "  Build it with: cd platform/qemu && ./setup.sh && make -C build"
        missing=1
    fi

    # Check kernel image
    if [[ ! -f "${KERNEL_IMAGE}" ]]; then
        error "Kernel image not found: ${KERNEL_IMAGE}"
        error "  Build it with: cd platform/buildroot && make"
        missing=1
    fi

    # Check rootfs image
    if [[ ! -f "${ROOTFS_IMAGE}" ]]; then
        error "Root filesystem not found: ${ROOTFS_IMAGE}"
        error "  Build it with: cd platform/buildroot && make"
        missing=1
    fi

    # Check shared directories exist
    if [[ ! -d "${DRIVER_DIR}" ]]; then
        warn "Driver directory not found: ${DRIVER_DIR}"
        warn "  9p mount for driver/ will fail in guest"
    fi

    if [[ ! -d "${APP_DIR}" ]]; then
        warn "App directory not found: ${APP_DIR}"
        warn "  9p mount for app/ will fail in guest"
    fi

    # Check KVM availability if requested
    if [[ "${ENABLE_KVM}" -eq 1 ]]; then
        if [[ ! -e /dev/kvm ]]; then
            warn "KVM not available (/dev/kvm not found)"
            warn "  Continuing without KVM (will be slower)"
            ENABLE_KVM=0
        elif [[ ! -r /dev/kvm ]] || [[ ! -w /dev/kvm ]]; then
            warn "KVM not accessible (check permissions on /dev/kvm)"
            warn "  Continuing without KVM (will be slower)"
            ENABLE_KVM=0
        fi
    fi

    if [[ "${missing}" -eq 1 ]]; then
        die "Missing prerequisites. See errors above."
    fi
}

# -----------------------------------------------------------------------------
# Parse command line arguments
# -----------------------------------------------------------------------------
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -h|--help)
                show_help
                exit 0
                ;;
            --headless)
                HEADLESS=1
                shift
                ;;
            --debug)
                DEBUG_MODE=1
                shift
                ;;
            --no-kvm)
                ENABLE_KVM=0
                shift
                ;;
            --memory)
                MEMORY="$2"
                shift 2
                ;;
            --cpus)
                CPUS="$2"
                shift 2
                ;;
            --ssh-port)
                SSH_PORT="$2"
                shift 2
                ;;
            --arch)
                TARGET_ARCH="$2"
                if [[ "${TARGET_ARCH}" != "x86_64" && "${TARGET_ARCH}" != "aarch64" ]]; then
                    die "Invalid architecture: ${TARGET_ARCH}. Use 'x86_64' or 'aarch64'."
                fi
                shift 2
                ;;
            --)
                shift
                EXTRA_ARGS+=("$@")
                break
                ;;
            -*)
                die "Unknown option: $1 (use --help for usage)"
                ;;
            *)
                # Treat as extra arg
                EXTRA_ARGS+=("$1")
                shift
                ;;
        esac
    done
}

# -----------------------------------------------------------------------------
# Build QEMU command
# -----------------------------------------------------------------------------
build_qemu_cmd() {
    local cmd=("${QEMU_BIN}")

    # BIOS/firmware directory (needed when running from build dir)
    cmd+=(-L "${QEMU_BUILD}/pc-bios")

    # Machine type (architecture-specific)
    cmd+=(-machine "${MACHINE_TYPE}")

    # CPU type
    if [[ "${ENABLE_KVM}" -eq 1 ]]; then
        cmd+=(-enable-kvm)
        # Use host CPU when KVM is available
        if [[ "${TARGET_ARCH}" == "x86_64" ]]; then
            cmd+=(-cpu host)
        else
            cmd+=(-cpu "${CPU_TYPE}")
        fi
    else
        cmd+=(-cpu "${CPU_TYPE}")
    fi

    # Memory and CPUs
    cmd+=(-m "${MEMORY}")
    cmd+=(-smp "${CPUS}")

    # Kernel and root filesystem
    cmd+=(-kernel "${KERNEL_IMAGE}")
    cmd+=(-drive "file=${ROOTFS_IMAGE},format=raw,if=virtio")
    cmd+=(-append "root=/dev/vda console=${CONSOLE}")

    # PhantomFPGA PCIe device - the star of the show
    cmd+=(-device phantomfpga)

    # Networking: user-mode with SSH port forwarding
    cmd+=(-netdev "user,id=net0,hostfwd=tcp::${SSH_PORT}-:22")
    cmd+=(-device virtio-net-pci,netdev=net0)

    # 9p virtfs mounts for driver/ and app/ directories
    if [[ -d "${DRIVER_DIR}" ]]; then
        cmd+=(-virtfs "local,path=${DRIVER_DIR},mount_tag=driver,security_model=mapped-xattr")
    fi
    if [[ -d "${APP_DIR}" ]]; then
        cmd+=(-virtfs "local,path=${APP_DIR},mount_tag=app,security_model=mapped-xattr")
    fi

    # Display mode
    if [[ "${HEADLESS}" -eq 1 ]]; then
        cmd+=(-nographic)
    else
        # Use serial console but keep graphical window
        cmd+=(-serial mon:stdio)
    fi

    # Debug mode: GDB server
    if [[ "${DEBUG_MODE}" -eq 1 ]]; then
        cmd+=(-gdb "tcp::${GDB_PORT}" -S)
        info "GDB server enabled on port ${GDB_PORT}"
        info "VM will wait for debugger connection before booting"
        info "Connect with: gdb -ex 'target remote :${GDB_PORT}'"
    fi

    # Extra arguments passed by user
    if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
        cmd+=("${EXTRA_ARGS[@]}")
    fi

    # Return the command array via printf (newline-separated for safety)
    printf '%s\n' "${cmd[@]}"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
    parse_args "$@"
    setup_arch_paths

    info "PhantomFPGA VM Launcher"
    info "======================="
    info "Target: ${TARGET_ARCH}"

    check_prerequisites

    # Build the QEMU command
    mapfile -t qemu_cmd < <(build_qemu_cmd)

    # Show what we're about to run
    info "Configuration:"
    info "  Arch:      ${TARGET_ARCH}"
    info "  Machine:   ${MACHINE_TYPE}"
    info "  Memory:    ${MEMORY}"
    info "  CPUs:      ${CPUS}"
    info "  KVM:       $([ "${ENABLE_KVM}" -eq 1 ] && echo "enabled" || echo "disabled")"
    info "  Headless:  $([ "${HEADLESS}" -eq 1 ] && echo "yes" || echo "no")"
    info "  Debug:     $([ "${DEBUG_MODE}" -eq 1 ] && echo "yes (port ${GDB_PORT})" || echo "no")"
    info "  SSH:       localhost:${SSH_PORT}"
    info ""
    info "Starting QEMU..."
    info "  (Press Ctrl-A X to exit in headless mode)"
    info ""

    # Run QEMU
    exec "${qemu_cmd[@]}"
}

main "$@"
