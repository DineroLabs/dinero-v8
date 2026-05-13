#!/bin/bash
set -euo pipefail

# Golden File Test Runner for DineroCoin CLI
# Ensures JSON output contract stability across versions

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-qt-free"
TEST_SCRIPT="$PROJECT_ROOT/tests/cli/test_golden_files.py"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_prerequisites() {
    log_info "Checking prerequisites..."
    
    # Check if build directory exists
    if [[ ! -d "$BUILD_DIR" ]]; then
        log_error "Build directory not found: $BUILD_DIR"
        log_info "Please run: mkdir -p build-qt-free && cd build-qt-free && cmake .. && make"
        exit 1
    fi
    
    # Check if CLI binary exists
    if [[ ! -f "$BUILD_DIR/dinero-cli-new" ]]; then
        log_error "CLI binary not found: $BUILD_DIR/dinero-cli-new"
        log_info "Please build the CLI: cd build-qt-free && make dinero-cli-new"
        exit 1
    fi
    
    # Check if daemon binary exists
    if [[ ! -f "$BUILD_DIR/dinerod" ]]; then
        log_error "Daemon binary not found: $BUILD_DIR/dinerod"
        log_info "Please build the daemon: cd build-qt-free && make dinerod"
        exit 1
    fi
    
    # Check if Python 3 is available
    if ! command -v python3 &> /dev/null; then
        log_error "Python 3 is required but not found"
        exit 1
    fi
    
    # Check if test script exists
    if [[ ! -f "$TEST_SCRIPT" ]]; then
        log_error "Test script not found: $TEST_SCRIPT"
        exit 1
    fi
    
    log_success "All prerequisites satisfied"
}

run_golden_tests() {
    local update_mode=""
    
    # Check if we're in update mode
    if [[ "${1:-}" == "--update" ]]; then
        update_mode="--update-golden"
        log_warning "Running in UPDATE mode - will recreate golden files"
    fi
    
    log_info "Starting golden file tests..."
    
    # Change to project root for consistent paths
    cd "$PROJECT_ROOT"
    
    # Run the Python test script
    if python3 "$TEST_SCRIPT" $update_mode; then
        log_success "All golden file tests passed!"
        return 0
    else
        log_error "Golden file tests failed!"
        return 1
    fi
}

show_help() {
    cat << EOF
Golden File Test Runner for DineroCoin CLI

USAGE:
    $0 [OPTIONS]

OPTIONS:
    --update    Update golden files with current output (use carefully!)
    --help      Show this help message

DESCRIPTION:
    This script runs comprehensive golden file tests to ensure the CLI's
    JSON output contract remains stable across versions. Golden files
    contain frozen expected outputs that are compared against actual
    CLI command results.

EXAMPLES:
    $0                  # Run tests against existing golden files
    $0 --update         # Update golden files with current output

GOLDEN FILES:
    Golden files are stored in: tests/cli/golden/
    
    Each test command has a corresponding .json file:
    - status.json           (dinero-cli status)
    - wallet_info.json      (dinero-cli wallet info)
    - wallet_balance.json   (dinero-cli wallet balance)
    - chain_info.json       (dinero-cli chain info)
    - net_info.json         (dinero-cli net info)
    - mining_info.json      (dinero-cli mining info)

INTEGRATION:
    Add to CI pipeline:
    - Run after each build to catch output regressions
    - Update golden files only when intentionally changing output format
    - Fail CI if golden tests fail (indicates breaking changes)

EOF
}

main() {
    case "${1:-}" in
        --help|-h)
            show_help
            exit 0
            ;;
        --update)
            check_prerequisites
            run_golden_tests --update
            ;;
        "")
            check_prerequisites
            run_golden_tests
            ;;
        *)
            log_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
}

main "$@"
