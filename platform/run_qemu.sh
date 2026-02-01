#!/bin/bash
#
# PhantomFPGA VM Launcher
# =======================
#
# This script launches a QEMU virtual machine with our custom PhantomFPGA
# PCIe device. But here's the thing: in real life, you won't have a nice
# script like this. You'll be staring at QEMU documentation, wondering why
# your VM won't boot, and questioning your career choices.
#
# So please, READ THIS SCRIPT. Understand what each flag does. The comments
# below explain everything - treat this as a QEMU tutorial, not a black box.
#
# When you're debugging real hardware issues, you'll need to know:
#   - How QEMU emulates different machine types
#   - How virtio devices work
#   - How to set up host-guest communication
#   - How to attach debuggers to running VMs
#
# TL;DR: Don't just run this script. Understand it. Your future self will
# thank you when something breaks at 2 AM.
#
# "May your registers be responsive and your interrupts timely."
#

set -e  # Exit on first error - fail fast, debug faster

# =============================================================================
# PATH SETUP
# =============================================================================
#
# We need to find our project files. In shell scripts, figuring out "where
# am I?" is surprisingly tricky. BASH_SOURCE[0] gives us the script's path
# even when sourced, and we use cd+pwd to resolve symlinks.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# =============================================================================
# DEFAULT CONFIGURATION
# =============================================================================
#
# These defaults work for most development scenarios. Override them with
# command-line flags when needed.

TARGET_ARCH="x86_64"          # Also supports "aarch64" for ARM64

QEMU_BUILD="${PROJECT_ROOT}/platform/qemu/build"
DRIVER_DIR="${PROJECT_ROOT}/driver"
APP_DIR="${PROJECT_ROOT}/app"

# These get set based on architecture (see setup_arch_paths)
QEMU_BIN=""
KERNEL_IMAGE=""
ROOTFS_IMAGE=""

# VM resources - 2G RAM and 2 CPUs is plenty for driver development
MEMORY="2G"
CPUS="2"

# Port forwarding - we map host:2222 -> guest:22 for SSH access
# This is QEMU user-mode networking, no root required
SSH_PORT="2222"
GDB_PORT="1234"

# Mode flags
HEADLESS=0      # Set to 1 for CI/automated testing (no display)
DEBUG_MODE=0    # Set to 1 to enable GDB server
ENABLE_KVM=1    # KVM makes things MUCH faster when available
EXTRA_ARGS=()   # Additional args passed through to QEMU

# =============================================================================
# HELP TEXT
# =============================================================================

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

WANT TO UNDERSTAND WHAT THIS SCRIPT DOES?
  Read the source! It's heavily commented to explain each QEMU option.
  Real driver developers need to understand their tools, not just use them.

  Key sections to study:
    - build_qemu_cmd()     How the QEMU command line is constructed
    - setup_arch_paths()   Architecture-specific settings
    - check_prerequisites() What needs to be in place before running

EOF
}

# =============================================================================
# LOGGING HELPERS
# =============================================================================

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

# =============================================================================
# ARCHITECTURE-SPECIFIC CONFIGURATION
# =============================================================================
#
# Different CPU architectures need different QEMU configurations. This is
# something you'll encounter constantly in embedded development - the same
# concepts (machine type, CPU model, console device) vary by platform.
#
# x86_64 (Intel/AMD):
#   - Machine: q35 - Modern Intel chipset with PCIe support
#   - CPU: Nehalem - A well-supported Intel CPU model
#   - Console: ttyS0 - Standard PC serial port
#   - Kernel: bzImage - Compressed x86 kernel format
#
# aarch64 (ARM64):
#   - Machine: virt - ARM's generic virtual platform
#   - CPU: cortex-a72 - Common ARM server CPU
#   - Console: ttyAMA0 - ARM PrimeCell UART
#   - Kernel: Image - Uncompressed ARM64 kernel

setup_arch_paths() {
    QEMU_BIN="${QEMU_BUILD}/qemu-system-${TARGET_ARCH}"

    case "${TARGET_ARCH}" in
        x86_64)
            KERNEL_IMAGE="${PROJECT_ROOT}/platform/images/bzImage"
            ROOTFS_IMAGE="${PROJECT_ROOT}/platform/images/rootfs.ext4"
            MACHINE_TYPE="q35"      # Modern Intel chipset, supports PCIe
            CPU_TYPE="Nehalem"      # Works without KVM, has enough features
            CONSOLE="ttyS0"         # Standard PC serial port
            ;;
        aarch64)
            KERNEL_IMAGE="${PROJECT_ROOT}/platform/images/Image"
            ROOTFS_IMAGE="${PROJECT_ROOT}/platform/images/rootfs-aarch64.ext4"
            MACHINE_TYPE="virt"     # ARM's generic virtualization platform
            CPU_TYPE="cortex-a72"   # Good balance of features and compatibility
            CONSOLE="ttyAMA0"       # ARM PrimeCell UART
            ;;
    esac
}

# =============================================================================
# PREREQUISITE CHECKS
# =============================================================================
#
# Before launching QEMU, we verify everything is in place. This saves you
# from cryptic QEMU error messages. In production, you'd have similar
# checks in your test harnesses and CI pipelines.

check_prerequisites() {
    local missing=0

    # Check QEMU binary exists and is executable
    if [[ ! -x "${QEMU_BIN}" ]]; then
        error "QEMU binary not found: ${QEMU_BIN}"
        error "  Build it with: cd platform/qemu && ./setup.sh && make -C build"
        missing=1
    fi

    # Check kernel image - this is what boots inside the VM
    if [[ ! -f "${KERNEL_IMAGE}" ]]; then
        error "Kernel image not found: ${KERNEL_IMAGE}"
        error "  Build it with: cd platform/buildroot && make"
        missing=1
    fi

    # Check root filesystem - contains the userspace (busybox, ssh, etc.)
    if [[ ! -f "${ROOTFS_IMAGE}" ]]; then
        error "Root filesystem not found: ${ROOTFS_IMAGE}"
        error "  Build it with: cd platform/buildroot && make"
        missing=1
    fi

    # Warn about missing shared directories (9p mounts will fail)
    if [[ ! -d "${DRIVER_DIR}" ]]; then
        warn "Driver directory not found: ${DRIVER_DIR}"
        warn "  9p mount for driver/ will fail in guest"
    fi

    if [[ ! -d "${APP_DIR}" ]]; then
        warn "App directory not found: ${APP_DIR}"
        warn "  9p mount for app/ will fail in guest"
    fi

    # Check KVM availability
    # KVM = Kernel-based Virtual Machine. When available, QEMU runs guest
    # code directly on the CPU instead of emulating every instruction.
    # This makes things 10-100x faster. On Linux, check /dev/kvm.
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

    # Check if SSH port is available - catch the "port already in use" error
    # before QEMU gives us a cryptic message
    if nc -z localhost "${SSH_PORT}" 2>/dev/null; then
        error "Port ${SSH_PORT} is already in use"
        error "  Another QEMU instance may be running. Kill it with:"
        error "    pkill qemu-system"
        error "  Or use a different port:"
        error "    $(basename "$0") --ssh-port 2223"
        missing=1
    fi

    if [[ "${missing}" -eq 1 ]]; then
        die "Missing prerequisites. See errors above."
    fi
}

# =============================================================================
# ARGUMENT PARSING
# =============================================================================

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
                EXTRA_ARGS+=("$1")
                shift
                ;;
        esac
    done
}

# =============================================================================
# BUILD QEMU COMMAND
# =============================================================================
#
# This is the heart of the script. Each QEMU flag is explained below.
# Understanding these options is essential for debugging VM issues.
#
# The QEMU command we're building looks something like:
#
#   qemu-system-x86_64 \
#     -L /path/to/pc-bios \           # Where to find firmware files
#     -machine q35 \                   # Emulate this machine/chipset
#     -enable-kvm \                    # Use hardware virtualization
#     -cpu host \                      # Pass through host CPU features
#     -m 2G \                          # 2 GB of RAM
#     -smp 2 \                         # 2 virtual CPUs
#     -kernel /path/to/bzImage \       # Boot this kernel directly
#     -drive file=rootfs.ext4,... \    # Root filesystem
#     -append "root=/dev/vda ..." \    # Kernel command line
#     -device phantomfpga \            # Our custom PCIe device!
#     -netdev user,... \               # User-mode networking
#     -device virtio-net-pci,... \     # Virtual network card
#     -virtfs local,... \              # Host directory sharing
#     -nographic                       # No GUI, serial console only
#

build_qemu_cmd() {
    local cmd=("${QEMU_BIN}")

    # -------------------------------------------------------------------------
    # FIRMWARE PATH (-L)
    # -------------------------------------------------------------------------
    # QEMU needs BIOS/firmware files (SeaBIOS for x86, etc.). When running
    # from a build directory, we need to tell it where these are.
    cmd+=(-L "${QEMU_BUILD}/pc-bios")

    # -------------------------------------------------------------------------
    # MACHINE TYPE (-machine)
    # -------------------------------------------------------------------------
    # This selects which hardware platform to emulate. Different machines
    # have different buses, interrupt controllers, and device layouts.
    #
    # For x86_64, "q35" is a modern Intel chipset with:
    #   - PCIe bus (not just PCI) - important for our PCIe device!
    #   - AHCI SATA controller
    #   - Modern interrupt routing (MSI/MSI-X support)
    #
    # For aarch64, "virt" is a minimal machine designed for virtualization.
    cmd+=(-machine "${MACHINE_TYPE}")

    # -------------------------------------------------------------------------
    # CPU MODEL (-cpu) and KVM (-enable-kvm)
    # -------------------------------------------------------------------------
    # With KVM enabled, guest code runs directly on the CPU. We use "-cpu host"
    # to expose all host CPU features to the guest.
    #
    # Without KVM, QEMU emulates every instruction in software. We use a
    # well-known CPU model (Nehalem for x86) that QEMU emulates accurately.
    if [[ "${ENABLE_KVM}" -eq 1 ]]; then
        cmd+=(-enable-kvm)
        if [[ "${TARGET_ARCH}" == "x86_64" ]]; then
            cmd+=(-cpu host)  # Use actual host CPU features
        else
            cmd+=(-cpu "${CPU_TYPE}")
        fi
    else
        cmd+=(-cpu "${CPU_TYPE}")  # Emulated CPU model
    fi

    # -------------------------------------------------------------------------
    # MEMORY AND CPUs (-m, -smp)
    # -------------------------------------------------------------------------
    # -m sets RAM size. Can use K, M, G suffixes.
    # -smp sets the number of virtual CPUs.
    #
    # For driver development, 2G RAM and 2 CPUs is plenty. Increase if you
    # need to test with more memory pressure or SMP-related issues.
    cmd+=(-m "${MEMORY}")
    cmd+=(-smp "${CPUS}")

    # -------------------------------------------------------------------------
    # KERNEL AND ROOTFS (-kernel, -drive, -append)
    # -------------------------------------------------------------------------
    # We boot the kernel directly (no bootloader). This is common for
    # embedded development and testing - it's faster and simpler.
    #
    # -kernel: The Linux kernel image to boot
    # -drive: The root filesystem (ext4 image), attached via virtio
    # -append: Kernel command line arguments
    #
    # The kernel command line tells Linux:
    #   root=/dev/vda   - Root filesystem is the virtio disk
    #   console=ttyS0   - Use serial port for console output
    cmd+=(-kernel "${KERNEL_IMAGE}")
    cmd+=(-drive "file=${ROOTFS_IMAGE},format=raw,if=virtio")
    cmd+=(-append "root=/dev/vda console=${CONSOLE}")

    # -------------------------------------------------------------------------
    # PHANTOMFPGA DEVICE (-device phantomfpga)
    # -------------------------------------------------------------------------
    # This is our custom PCIe device! It's defined in:
    #   platform/qemu/src/hw/misc/phantomfpga.c
    #
    # QEMU's -device option instantiates a device model. Our device appears
    # as a PCIe endpoint with vendor:device = 0x0DAD:0xF00D.
    #
    # This is the device you'll be writing a driver for!
    cmd+=(-device phantomfpga)

    # -------------------------------------------------------------------------
    # NETWORKING (-netdev, -device virtio-net-pci)
    # -------------------------------------------------------------------------
    # QEMU user-mode networking: simple, no root required, but limited.
    # The VM gets a private 10.0.2.x address and can access the internet
    # through NAT. The host can't directly reach the VM, so we set up
    # port forwarding for SSH.
    #
    # -netdev user,id=net0,hostfwd=tcp::2222-:22
    #   Creates a user-mode network backend named "net0"
    #   Forwards host port 2222 to guest port 22 (SSH)
    #
    # -device virtio-net-pci,netdev=net0
    #   Creates a virtio network card connected to "net0"
    #   virtio is faster than emulating real hardware (e1000, rtl8139)
    cmd+=(-netdev "user,id=net0,hostfwd=tcp::${SSH_PORT}-:22")
    cmd+=(-device virtio-net-pci,netdev=net0)

    # -------------------------------------------------------------------------
    # DIRECTORY SHARING (-virtfs)
    # -------------------------------------------------------------------------
    # 9p/virtfs lets the guest mount directories from the host. This is
    # incredibly useful for development - edit files on the host, build
    # and test in the guest, without copying files back and forth.
    #
    # -virtfs local,path=...,mount_tag=driver,security_model=mapped-xattr
    #   local          - Use the local filesystem
    #   path=...       - Host directory to share
    #   mount_tag=     - Name the guest uses to mount (like a volume label)
    #   security_model - How to handle permissions (mapped-xattr stores
    #                    guest permissions in extended attributes)
    #
    # In the guest, mount with:
    #   mount -t 9p -o trans=virtio driver /mnt/driver
    #
    # Our guest's /etc/fstab auto-mounts these at boot.
    if [[ -d "${DRIVER_DIR}" ]]; then
        cmd+=(-virtfs "local,path=${DRIVER_DIR},mount_tag=driver,security_model=mapped-xattr")
    fi
    if [[ -d "${APP_DIR}" ]]; then
        cmd+=(-virtfs "local,path=${APP_DIR},mount_tag=app,security_model=mapped-xattr")
    fi

    # -------------------------------------------------------------------------
    # DISPLAY MODE (-nographic, -serial)
    # -------------------------------------------------------------------------
    # -nographic: No graphical window, console I/O goes to terminal
    #             Exit with Ctrl-A X
    #
    # -serial mon:stdio: Connect serial port to terminal, with QEMU monitor
    #                    accessible via Ctrl-A C
    #
    # For headless CI, we want -nographic. For interactive development,
    # we might want a window for graphical output plus serial on stdio.
    if [[ "${HEADLESS}" -eq 1 ]]; then
        cmd+=(-nographic)
    else
        cmd+=(-serial mon:stdio)
    fi

    # -------------------------------------------------------------------------
    # DEBUG MODE (-gdb, -S)
    # -------------------------------------------------------------------------
    # -gdb tcp::1234  - Start a GDB server on port 1234
    # -S              - Don't start CPU at startup (wait for debugger)
    #
    # This lets you attach GDB and debug the kernel or your driver:
    #   gdb vmlinux -ex "target remote :1234"
    #
    # You can set breakpoints, inspect memory, step through code - the
    # whole nine yards. Essential for tracking down kernel panics.
    if [[ "${DEBUG_MODE}" -eq 1 ]]; then
        cmd+=(-gdb "tcp::${GDB_PORT}" -S)
        info "GDB server enabled on port ${GDB_PORT}"
        info "VM will wait for debugger connection before booting"
        info "Connect with: gdb -ex 'target remote :${GDB_PORT}'"
    fi

    # Pass through any extra arguments the user provided
    if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
        cmd+=("${EXTRA_ARGS[@]}")
    fi

    # Return the command as newline-separated strings
    printf '%s\n' "${cmd[@]}"
}

# =============================================================================
# MAIN
# =============================================================================

main() {
    parse_args "$@"
    setup_arch_paths

    info "PhantomFPGA VM Launcher"
    info "======================="
    info "Target: ${TARGET_ARCH}"

    check_prerequisites

    # Build the QEMU command
    mapfile -t qemu_cmd < <(build_qemu_cmd)

    # Show configuration summary
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

    # Replace this process with QEMU (exec = no fork)
    exec "${qemu_cmd[@]}"
}

main "$@"
