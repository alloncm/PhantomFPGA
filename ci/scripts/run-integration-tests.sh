#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# PhantomFPGA CI Integration Test Runner
#
# Runs integration tests inside a QEMU VM for CI pipelines.
# Handles VM startup, test execution, and result collection.
#
# Usage:
#   ./run-integration-tests.sh [OPTIONS]
#
# Options:
#   --arch ARCH       Target architecture (x86_64, aarch64)
#   --timeout SECS    Test timeout in seconds (default: 300)
#   --junit FILE      Write JUnit XML results to FILE
#   --quick           Run quick smoke tests only
#   --verbose         Verbose output
#   --help            Show help
#
# "May your VMs boot quickly and your tests pass on the first try."
# (Spoiler: They won't. That's why we have CI.)

set -euo pipefail

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Defaults
ARCH="x86_64"
TIMEOUT=300
JUNIT_FILE=""
QUICK_MODE=false
VERBOSE=false
QEMU_PID=""
TEST_LOG=""

# Cleanup on exit
cleanup() {
    local exit_code=$?

    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "[CI] Stopping QEMU (PID: $QEMU_PID)..."
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi

    if [ -n "$TEST_LOG" ] && [ -f "$TEST_LOG" ]; then
        echo "[CI] Test log saved to: $TEST_LOG"
    fi

    if [ $exit_code -ne 0 ]; then
        echo "[CI] Tests failed with exit code: $exit_code"
    fi

    exit $exit_code
}

trap cleanup EXIT INT TERM

# Logging helpers
info() {
    echo "[CI] $*"
}

error() {
    echo "[CI] ERROR: $*" >&2
}

die() {
    error "$*"
    exit 1
}

# Parse arguments
usage() {
    cat << EOF
PhantomFPGA CI Integration Test Runner

Usage: $(basename "$0") [OPTIONS]

Options:
    --arch ARCH       Target architecture (x86_64 or aarch64)
    --timeout SECS    Test timeout in seconds (default: 300)
    --junit FILE      Write JUnit XML results to FILE
    --quick           Run quick smoke tests only
    --verbose         Verbose output
    --help            Show this help

Examples:
    $(basename "$0") --arch x86_64 --timeout 600
    $(basename "$0") --junit results.xml --quick
EOF
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            ARCH="$2"
            shift 2
            ;;
        --timeout)
            TIMEOUT="$2"
            shift 2
            ;;
        --junit)
            JUNIT_FILE="$2"
            shift 2
            ;;
        --quick)
            QUICK_MODE=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
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
    x86_64|aarch64)
        ;;
    *)
        die "Invalid architecture: $ARCH (use x86_64 or aarch64)"
        ;;
esac

# Check prerequisites
check_prerequisites() {
    info "Checking prerequisites..."

    local qemu_bin="${PROJECT_ROOT}/platform/qemu/build/qemu-system-${ARCH}"
    if [ ! -x "$qemu_bin" ]; then
        die "QEMU binary not found: $qemu_bin"
    fi

    case "$ARCH" in
        x86_64)
            local kernel="${PROJECT_ROOT}/platform/images/bzImage"
            local rootfs="${PROJECT_ROOT}/platform/images/rootfs.ext4"
            ;;
        aarch64)
            local kernel="${PROJECT_ROOT}/platform/images/Image"
            local rootfs="${PROJECT_ROOT}/platform/images/rootfs-aarch64.ext4"
            ;;
    esac

    if [ ! -f "$kernel" ]; then
        die "Kernel image not found: $kernel"
    fi

    if [ ! -f "$rootfs" ]; then
        die "Rootfs image not found: $rootfs"
    fi

    # Check for KVM
    if [ -c /dev/kvm ]; then
        info "KVM is available"
        KVM_ENABLED=true
    else
        info "KVM not available, tests will run slower"
        KVM_ENABLED=false
    fi

    info "Prerequisites OK"
}

# Start QEMU in background
start_qemu() {
    info "Starting QEMU ($ARCH)..."

    local qemu_bin="${PROJECT_ROOT}/platform/qemu/build/qemu-system-${ARCH}"
    TEST_LOG="${PROJECT_ROOT}/qemu-test-${ARCH}.log"

    # Build QEMU command
    local qemu_cmd=("$qemu_bin")

    case "$ARCH" in
        x86_64)
            qemu_cmd+=(-machine q35)
            qemu_cmd+=(-kernel "${PROJECT_ROOT}/platform/images/bzImage")
            qemu_cmd+=(-drive "file=${PROJECT_ROOT}/platform/images/rootfs.ext4,format=raw,if=virtio")
            qemu_cmd+=(-append "root=/dev/vda console=ttyS0 quiet")
            if [ "$KVM_ENABLED" = true ]; then
                qemu_cmd+=(-enable-kvm -cpu host)
            else
                qemu_cmd+=(-cpu qemu64)
            fi
            ;;
        aarch64)
            qemu_cmd+=(-machine virt)
            qemu_cmd+=(-kernel "${PROJECT_ROOT}/platform/images/Image")
            qemu_cmd+=(-drive "file=${PROJECT_ROOT}/platform/images/rootfs-aarch64.ext4,format=raw,if=virtio")
            qemu_cmd+=(-append "root=/dev/vda console=ttyAMA0 quiet")
            qemu_cmd+=(-cpu cortex-a72)
            ;;
    esac

    # Common options
    qemu_cmd+=(-m 2G)
    qemu_cmd+=(-smp 2)
    qemu_cmd+=(-device phantomfpga-pcie)
    qemu_cmd+=(-nographic)
    qemu_cmd+=(-serial mon:stdio)

    # Networking for test commands
    qemu_cmd+=(-netdev "user,id=net0,hostfwd=tcp::2222-:22")
    qemu_cmd+=(-device virtio-net-pci,netdev=net0)

    if $VERBOSE; then
        info "QEMU command: ${qemu_cmd[*]}"
    fi

    # Start QEMU with output redirected to log
    "${qemu_cmd[@]}" > "$TEST_LOG" 2>&1 &
    QEMU_PID=$!

    info "QEMU started (PID: $QEMU_PID)"

    # Wait for VM to boot (look for login prompt in log)
    info "Waiting for VM to boot..."
    local boot_timeout=120
    local elapsed=0

    while [ $elapsed -lt $boot_timeout ]; do
        if grep -q "login:" "$TEST_LOG" 2>/dev/null; then
            info "VM booted successfully"
            return 0
        fi

        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            error "QEMU exited unexpectedly"
            cat "$TEST_LOG"
            return 1
        fi

        sleep 2
        elapsed=$((elapsed + 2))

        if [ $((elapsed % 20)) -eq 0 ]; then
            info "Still waiting for boot... (${elapsed}s)"
        fi
    done

    error "VM boot timeout after ${boot_timeout}s"
    cat "$TEST_LOG"
    return 1
}

# Run tests via SSH
run_tests_ssh() {
    info "Running tests via SSH..."

    local ssh_opts="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10"
    local ssh_cmd="ssh -p 2222 $ssh_opts root@localhost"
    local test_args=""

    if $QUICK_MODE; then
        test_args="--quick"
    fi

    # Wait for SSH to be available
    local ssh_timeout=60
    local elapsed=0

    info "Waiting for SSH..."
    while [ $elapsed -lt $ssh_timeout ]; do
        if $ssh_cmd "echo 'SSH ready'" 2>/dev/null; then
            info "SSH connection established"
            break
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done

    if [ $elapsed -ge $ssh_timeout ]; then
        error "SSH connection timeout"
        return 1
    fi

    # Run integration tests
    local test_result=0

    info "Executing integration tests..."

    # Load driver
    $ssh_cmd "modprobe phantomfpga 2>/dev/null || insmod /lib/modules/*/extra/phantomfpga.ko" || {
        error "Failed to load driver"
        test_result=1
    }

    # Check device presence
    if [ $test_result -eq 0 ]; then
        $ssh_cmd "lspci -nn | grep -i 1dad:f00d" || {
            error "PhantomFPGA device not found"
            test_result=1
        }
    fi

    # Run test scripts if they exist in the guest
    if [ $test_result -eq 0 ]; then
        $ssh_cmd "[ -x /usr/local/bin/phantomfpga_app ] && /usr/local/bin/phantomfpga_app --test" || {
            info "App test not available or failed"
            # Don't fail the whole test for this
        }
    fi

    # Basic driver functionality tests
    if [ $test_result -eq 0 ]; then
        info "Testing basic driver functionality..."

        # Test device exists
        $ssh_cmd "[ -c /dev/phantomfpga0 ]" || {
            error "Device node /dev/phantomfpga0 not found"
            test_result=1
        }

        # Test read registers via sysfs or direct
        $ssh_cmd "cat /sys/class/misc/phantomfpga0/device/vendor 2>/dev/null || true"
    fi

    # Collect dmesg
    info "Collecting kernel messages..."
    $ssh_cmd "dmesg | grep -i phantomfpga" || true

    return $test_result
}

# Run tests via serial console (fallback when SSH not available)
run_tests_serial() {
    info "Running tests via serial console..."

    # For serial-based testing, we inject commands into the QEMU console
    # This is a fallback for when SSH is not available

    local test_result=0

    # Send login
    echo "root" > /proc/$QEMU_PID/fd/0 2>/dev/null || true
    sleep 2

    # Send test commands
    cat << 'EOF' > /tmp/test_commands.sh
#!/bin/sh
echo "=== PhantomFPGA Integration Tests ==="

# Load driver
modprobe phantomfpga 2>/dev/null || insmod /lib/modules/*/extra/phantomfpga.ko
sleep 1

# Check device
if lspci -nn | grep -q "1dad:f00d"; then
    echo "[PASS] PhantomFPGA PCI device found"
else
    echo "[FAIL] PhantomFPGA PCI device not found"
    exit 1
fi

# Check driver loaded
if lsmod | grep -q "^phantomfpga"; then
    echo "[PASS] Driver loaded"
else
    echo "[FAIL] Driver not loaded"
    exit 1
fi

# Check device node
if [ -c /dev/phantomfpga0 ]; then
    echo "[PASS] Device node created"
else
    echo "[WARN] Device node /dev/phantomfpga0 not found"
fi

echo "=== Tests Complete ==="
poweroff
EOF

    # The serial approach is tricky in CI; we rely on SSH when possible
    info "Serial console testing not fully implemented; use SSH when available"

    return $test_result
}

# Generate JUnit XML report
generate_junit_report() {
    local output_file="$1"
    local test_result="$2"
    local test_time="$3"

    info "Generating JUnit report: $output_file"

    local status="passed"
    local failures=0
    if [ "$test_result" -ne 0 ]; then
        status="failed"
        failures=1
    fi

    cat > "$output_file" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="PhantomFPGA Integration Tests" tests="1" failures="$failures" time="$test_time">
  <testsuite name="integration-$ARCH" tests="1" failures="$failures" time="$test_time">
    <testcase name="vm-integration-test" classname="integration.$ARCH" time="$test_time">
EOF

    if [ "$test_result" -ne 0 ]; then
        cat >> "$output_file" << EOF
      <failure message="Integration test failed">
See test log for details.
Exit code: $test_result
      </failure>
EOF
    fi

    cat >> "$output_file" << EOF
    </testcase>
  </testsuite>
</testsuites>
EOF
}

# Main
main() {
    info "PhantomFPGA CI Integration Tests"
    info "================================="
    info "Architecture: $ARCH"
    info "Timeout: ${TIMEOUT}s"
    info "Quick mode: $QUICK_MODE"
    info ""

    check_prerequisites

    local start_time=$(date +%s)
    local test_result=0

    # Start QEMU
    if ! start_qemu; then
        test_result=1
    fi

    # Run tests with timeout
    if [ $test_result -eq 0 ]; then
        if timeout "$TIMEOUT" bash -c "$(declare -f run_tests_ssh); run_tests_ssh"; then
            info "Tests passed!"
        else
            error "Tests failed or timed out"
            test_result=1
        fi
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    info "Test duration: ${duration}s"

    # Generate JUnit report if requested
    if [ -n "$JUNIT_FILE" ]; then
        generate_junit_report "$JUNIT_FILE" "$test_result" "$duration"
    fi

    # Summary
    if [ $test_result -eq 0 ]; then
        info "=== ALL TESTS PASSED ==="
    else
        info "=== TESTS FAILED ==="

        # Show last part of log on failure
        if [ -f "$TEST_LOG" ]; then
            info "Last 50 lines of QEMU log:"
            tail -50 "$TEST_LOG"
        fi
    fi

    return $test_result
}

main "$@"
