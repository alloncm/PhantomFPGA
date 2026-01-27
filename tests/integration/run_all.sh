#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# PhantomFPGA Integration Test Runner
#
# Runs all integration tests and reports results.
# Suitable for CI/CD pipelines.
#
# Usage: ./run_all.sh [OPTIONS]
#
# Options:
#   --quick     Run tests in quick mode (shorter durations)
#   --verbose   Show more output from individual tests
#   --stop-on-fail  Stop at first failing test suite
#   --junit     Generate JUnit XML report (for CI systems)
#   --help      Show this help
#
# Exit codes:
#   0   All tests passed
#   1   Some tests failed
#   2   Test infrastructure error
#
# "In theory, there is no difference between theory and practice.
#  In practice, there is."
#   -- Yogi Berra (or Benjamin Brewster, or everyone who's written tests)

set -u

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JUNIT_OUTPUT=""
STOP_ON_FAIL=false
QUICK_MODE=false
VERBOSE=false

# Test suites to run (in order)
TEST_SUITES=(
    "test_driver.sh"
    "test_streaming.sh"
    "test_faults.sh"
)

# Results tracking
declare -A SUITE_RESULTS
TOTAL_PASSED=0
TOTAL_FAILED=0
TOTAL_SKIPPED=0

# Timing
START_TIME=0
END_TIME=0

# Colors
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BLUE='\033[0;34m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    BOLD=''
    NC=''
fi

# --------------------------------------------------------------------------
# Helper Functions
# --------------------------------------------------------------------------

usage() {
    cat << EOF
PhantomFPGA Integration Test Runner

Usage: $(basename "$0") [OPTIONS]

Options:
    --quick          Run tests in quick mode (shorter durations)
    --verbose        Show verbose output from test suites
    --stop-on-fail   Stop at first failing test suite
    --junit FILE     Generate JUnit XML report
    --help           Show this help

Exit codes:
    0   All tests passed
    1   Some tests failed
    2   Test infrastructure error

Examples:
    $(basename "$0")                    # Run all tests
    $(basename "$0") --quick            # Quick smoke test
    $(basename "$0") --junit report.xml # Generate CI report
EOF
}

log_header() {
    echo ""
    echo -e "${BOLD}========================================${NC}"
    echo -e "${BOLD}$*${NC}"
    echo -e "${BOLD}========================================${NC}"
    echo ""
}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $*"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $*"
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_skip() {
    echo -e "${YELLOW}[SKIP]${NC} $*"
}

# Get current timestamp in seconds
timestamp() {
    date +%s
}

# Format duration from seconds
format_duration() {
    local seconds=$1
    local minutes=$((seconds / 60))
    local secs=$((seconds % 60))
    if [ $minutes -gt 0 ]; then
        echo "${minutes}m ${secs}s"
    else
        echo "${secs}s"
    fi
}

# Check if we're running as root
check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        log_warn "Not running as root. Some tests may fail."
        log_info "Consider running with sudo for best results."
        return 1
    fi
    return 0
}

# Check test environment
check_environment() {
    log_info "Checking test environment..."

    # Check kernel version
    log_info "Kernel: $(uname -r)"

    # Check if driver module exists
    if [ -f "/lib/modules/$(uname -r)/extra/phantomfpga.ko" ] || modinfo phantomfpga >/dev/null 2>&1; then
        log_info "Driver module: found"
    else
        log_warn "Driver module: not found"
        log_warn "Tests may fail - ensure driver is built and installed"
    fi

    # Check if app exists
    if [ -x "/usr/local/bin/phantomfpga_app" ]; then
        log_info "App binary: found"
    else
        log_warn "App binary: not found"
        log_warn "Some tests will be limited"
    fi

    # Check PCI device (if lspci available)
    if command -v lspci >/dev/null 2>&1; then
        if lspci -nn 2>/dev/null | grep -q "1dad:f00d"; then
            log_info "PCI device: found"
        else
            log_warn "PCI device: not found"
            log_warn "Tests will fail without PhantomFPGA device"
        fi
    fi

    return 0
}

# Run a single test suite
run_suite() {
    local suite=$1
    local suite_path="${SCRIPT_DIR}/${suite}"
    local suite_name="${suite%.sh}"
    local suite_args=""
    local suite_start
    local suite_end
    local exit_code

    if [ ! -x "$suite_path" ]; then
        log_warn "Test suite not found or not executable: $suite_path"
        SUITE_RESULTS[$suite_name]="SKIP"
        TOTAL_SKIPPED=$((TOTAL_SKIPPED + 1))
        return 0
    fi

    # Build arguments
    if $QUICK_MODE && [ "$suite" != "test_driver.sh" ]; then
        suite_args="--quick"
    fi

    log_header "Running: $suite_name"

    suite_start=$(timestamp)

    if $VERBOSE; then
        "$suite_path" $suite_args
        exit_code=$?
    else
        # Capture output but show summary
        local output
        output=$("$suite_path" $suite_args 2>&1)
        exit_code=$?

        # Show filtered output (summary lines)
        echo "$output" | grep -E "^\[(PASS|FAIL|WARN|INFO|TEST)\]|^Tests|^---"
    fi

    suite_end=$(timestamp)
    local suite_duration=$((suite_end - suite_start))

    if [ $exit_code -eq 0 ]; then
        SUITE_RESULTS[$suite_name]="PASS"
        TOTAL_PASSED=$((TOTAL_PASSED + 1))
        log_pass "$suite_name completed in $(format_duration $suite_duration)"
    else
        SUITE_RESULTS[$suite_name]="FAIL"
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        log_fail "$suite_name failed (exit code: $exit_code)"

        if $VERBOSE; then
            log_info "Recent dmesg:"
            dmesg | tail -20
        fi

        if $STOP_ON_FAIL; then
            log_fail "Stopping due to --stop-on-fail"
            return 1
        fi
    fi

    return 0
}

# Generate JUnit XML report
generate_junit_report() {
    local output_file=$1
    local total=$((TOTAL_PASSED + TOTAL_FAILED + TOTAL_SKIPPED))
    local duration=$((END_TIME - START_TIME))

    log_info "Generating JUnit report: $output_file"

    cat > "$output_file" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="PhantomFPGA Integration Tests" tests="$total" failures="$TOTAL_FAILED" skipped="$TOTAL_SKIPPED" time="$duration">
EOF

    for suite in "${TEST_SUITES[@]}"; do
        local suite_name="${suite%.sh}"
        local result="${SUITE_RESULTS[$suite_name]:-SKIP}"
        local failure=""

        if [ "$result" = "FAIL" ]; then
            failure="<failure message=\"Test suite failed\">See test output for details</failure>"
        elif [ "$result" = "SKIP" ]; then
            failure="<skipped message=\"Test suite skipped\"/>"
        fi

        cat >> "$output_file" << EOF
  <testsuite name="$suite_name" tests="1" failures="$([ "$result" = "FAIL" ] && echo 1 || echo 0)" skipped="$([ "$result" = "SKIP" ] && echo 1 || echo 0)">
    <testcase name="$suite_name" classname="integration">
      $failure
    </testcase>
  </testsuite>
EOF
    done

    cat >> "$output_file" << EOF
</testsuites>
EOF
}

# Print final summary
print_summary() {
    local total=$((TOTAL_PASSED + TOTAL_FAILED + TOTAL_SKIPPED))
    local duration=$((END_TIME - START_TIME))

    log_header "Test Summary"

    echo "Test suites:"
    for suite in "${TEST_SUITES[@]}"; do
        local suite_name="${suite%.sh}"
        local result="${SUITE_RESULTS[$suite_name]:-SKIP}"
        case "$result" in
            PASS) echo -e "  ${GREEN}[PASS]${NC} $suite_name" ;;
            FAIL) echo -e "  ${RED}[FAIL]${NC} $suite_name" ;;
            SKIP) echo -e "  ${YELLOW}[SKIP]${NC} $suite_name" ;;
        esac
    done

    echo ""
    echo "Results:"
    echo -e "  Total:   $total suites"
    echo -e "  Passed:  ${GREEN}$TOTAL_PASSED${NC}"
    echo -e "  Failed:  ${RED}$TOTAL_FAILED${NC}"
    echo -e "  Skipped: ${YELLOW}$TOTAL_SKIPPED${NC}"
    echo ""
    echo "Duration: $(format_duration $duration)"
    echo ""

    if [ $TOTAL_FAILED -gt 0 ]; then
        echo -e "${RED}${BOLD}OVERALL: FAILED${NC}"
        echo ""
        echo "Failed suites may indicate:"
        echo "  - Driver implementation issues"
        echo "  - Missing device (QEMU not running?)"
        echo "  - Permission problems (run as root)"
        echo "  - Hardware/emulation issues"
        return 1
    elif [ $TOTAL_PASSED -eq 0 ]; then
        echo -e "${YELLOW}${BOLD}OVERALL: NO TESTS RAN${NC}"
        return 2
    else
        echo -e "${GREEN}${BOLD}OVERALL: PASSED${NC}"
        return 0
    fi
}

# Cleanup on exit
cleanup() {
    # Ensure driver is in a clean state
    if lsmod | grep -q "^phantomfpga "; then
        log_info "Leaving driver loaded for inspection"
    fi
}

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

main() {
    # Parse arguments
    while [ $# -gt 0 ]; do
        case "$1" in
            --quick)
                QUICK_MODE=true
                ;;
            --verbose)
                VERBOSE=true
                ;;
            --stop-on-fail)
                STOP_ON_FAIL=true
                ;;
            --junit)
                shift
                if [ $# -eq 0 ]; then
                    echo "Error: --junit requires a file path"
                    exit 2
                fi
                JUNIT_OUTPUT="$1"
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
                echo "Unknown option: $1"
                usage
                exit 2
                ;;
        esac
        shift
    done

    trap cleanup EXIT

    log_header "PhantomFPGA Integration Tests"

    echo "Configuration:"
    echo "  Quick mode:     $QUICK_MODE"
    echo "  Verbose:        $VERBOSE"
    echo "  Stop on fail:   $STOP_ON_FAIL"
    echo "  JUnit output:   ${JUNIT_OUTPUT:-none}"
    echo ""

    # Pre-flight checks
    check_root || true
    check_environment || true

    START_TIME=$(timestamp)

    # Run all test suites
    for suite in "${TEST_SUITES[@]}"; do
        if ! run_suite "$suite"; then
            break
        fi
    done

    END_TIME=$(timestamp)

    # Generate JUnit report if requested
    if [ -n "$JUNIT_OUTPUT" ]; then
        generate_junit_report "$JUNIT_OUTPUT"
    fi

    # Print summary and return appropriate exit code
    print_summary
    exit $?
}

# Run main
main "$@"
