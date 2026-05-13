#!/usr/bin/env bash
set -euo pipefail

echo "🧪 Dinero Test Rig - Comprehensive Validation Suite"
echo "=================================================="

# Configuration
BIN_DIR="${BIN_DIR:-./build/bin}"
TEST_DIR="${TEST_DIR:-./build/tests}"
DAEMON="${BIN_DIR}/dinerod"
CLI="${BIN_DIR}/dinero-cli"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() { echo -e "${BLUE}ℹ️  $1${NC}"; }
log_success() { echo -e "${GREEN}✅ $1${NC}"; }
log_warning() { echo -e "${YELLOW}⚠️  $1${NC}"; }
log_error() { echo -e "${RED}❌ $1${NC}"; }

# Check dependencies
check_deps() {
    log_info "Checking dependencies..."
    
    local missing=()
    for cmd in jq curl lsof; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done
    
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing dependencies: ${missing[*]}"
        log_info "Install with: brew install ${missing[*]}"
        exit 1
    fi
    
    if [[ ! -f "$DAEMON" ]]; then
        log_error "Daemon not found at $DAEMON"
        log_info "Build with: cmake --build build -j8"
        exit 1
    fi
    
    log_success "Dependencies OK"
}

# Run unit tests
run_unit_tests() {
    log_info "Running unit tests..."
    
    if [[ -f "$TEST_DIR/test_chainparams" ]]; then
        "$TEST_DIR/test_chainparams" && log_success "Unit tests passed" || log_error "Unit tests failed"
    else
        log_warning "Unit tests not built (test_chainparams not found)"
    fi
}

# Run integration tests
run_integration_tests() {
    log_info "Running integration tests..."
    
    local tests=(
        "smoke.sh:30-second smoke test"
        "mine_one.sh:Mining + wallet credit test"
        "contract_check.sh:RPC schema validation"
        "persistence_check.sh:CIC binding and persistence"
        "crash_recovery.sh:Crash/recovery durability"
        "mempool_test.sh:Mempool admission policy"
        "matrix.sh:Backend compatibility matrix"
    )
    
    local passed=0
    local failed=0
    
    for test_entry in "${tests[@]}"; do
        IFS=':' read -r script description <<< "$test_entry"
        
        log_info "Running $description..."
        
        if timeout 120 bash "scripts/dev/$script" 2>&1 | tee "test_results/${script%.sh}.log"; then
            log_success "$description passed"
            ((passed++))
        else
            log_error "$description failed"
            ((failed++))
        fi
        
        # Clean up any leftover processes
        pkill -f "dinerod.*--rpcport" || true
        sleep 1
    done
    
    log_info "Integration test summary: $passed passed, $failed failed"
    return $failed
}

# Performance benchmarks
run_benchmarks() {
    log_info "Running performance benchmarks..."
    
    # Simple RPC latency test
    DATADIR="./bench_data"
    rm -rf "$DATADIR"
    
    "$DAEMON" --datadir="$DATADIR" --regtest --rpcport=20996 --printtoconsole > "$DATADIR/bench.log" 2>&1 &
    PID=$!
    sleep 3
    
    COOKIE_FILE="$DATADIR/regtest/.cookie"
    if [[ -f "$COOKIE_FILE" ]]; then
        AUTH="$(cat "$COOKIE_FILE")"
        URL="http://127.0.0.1:20996/"
        
        log_info "Measuring RPC latency (10 requests)..."
        start_time=$(date +%s%N)
        for i in {1..10}; do
            curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" \
                -d '{"jsonrpc":"2.0","id":'$i',"method":"getblockchaininfo","params":[]}' > /dev/null
        done
        end_time=$(date +%s%N)
        
        latency=$(( (end_time - start_time) / 10000000 )) # Convert to ms
        log_success "Average RPC latency: ${latency}ms"
    else
        log_warning "Benchmark daemon failed to start"
    fi
    
    kill -TERM $PID 2>/dev/null || true; wait $PID 2>/dev/null || true
    rm -rf "$DATADIR"
}

# Generate test report
generate_report() {
    log_info "Generating test report..."
    
    {
        echo "# Dinero Test Report"
        echo "Generated: $(date)"
        echo ""
        echo "## Test Results"
        
        for log_file in test_results/*.log; do
            if [[ -f "$log_file" ]]; then
                test_name=$(basename "$log_file" .log)
                echo "### $test_name"
                echo '```'
                tail -20 "$log_file"
                echo '```'
                echo ""
            fi
        done
        
        echo "## System Information"
        echo "- OS: $(uname -s) $(uname -r)"
        echo "- Architecture: $(uname -m)"
        echo "- Daemon: $DAEMON"
        echo "- Build: $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
        
    } > test_results/report.md
    
    log_success "Report generated: test_results/report.md"
}

# Main execution
main() {
    # Setup
    mkdir -p test_results
    cd "$(dirname "$0")/../.."  # Go to repo root
    
    # Run test suite
    check_deps
    
    log_info "Starting comprehensive test suite..."
    
    run_unit_tests
    
    local integration_failures=0
    run_integration_tests || integration_failures=$?
    
    run_benchmarks
    
    generate_report
    
    # Summary
    echo ""
    echo "🎯 Test Suite Complete"
    echo "======================"
    
    if [[ $integration_failures -eq 0 ]]; then
        log_success "All tests passed! 🎉"
        exit 0
    else
        log_error "$integration_failures integration tests failed"
        log_info "Check test_results/ for detailed logs"
        exit 1
    fi
}

# Handle interruption
trap 'pkill -f "dinerod.*--rpcport" || true; exit 130' INT TERM

main "$@"
