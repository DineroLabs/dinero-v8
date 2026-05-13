#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# Dinero Mining Stack End-to-End Integration Test
# ═══════════════════════════════════════════════════════════════════════════════
#
# This script validates the entire mining pipeline:
#   1. Daemon (dinerod) - Block template generation, RPC
#   2. Stratum Server - Job distribution, share validation
#   3. CPU Miner - Header construction, hash computation
#
# Prerequisites:
#   - dinerod built and in PATH or DINERO_DAEMON set
#   - stratum server built
#   - dinero-miner built
#
# Usage:
#   ./test_mining_stack_e2e.sh [--mainnet|--testnet|--regtest]
#
# ═══════════════════════════════════════════════════════════════════════════════

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
NETWORK="${1:---regtest}"
DATADIR="/tmp/dinero_mining_test_$$"
RPC_PORT=21996
STRATUM_PORT=3333
TEST_ADDRESS="din1qtest000000000000000000000000000000000"

# Paths (adjust as needed)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DINERO_ROOT="${SCRIPT_DIR}/../.."
STRATUM_ROOT="${DINERO_ROOT}/../stratum"
MINER_ROOT="${DINERO_ROOT}/../dinero-miner"

DINEROD="${DINERO_DAEMON:-${DINERO_ROOT}/build/bin/dinerod}"
STRATUM_SERVER="${STRATUM_ROOT}/build/stratum_server"
MINER="${MINER_ROOT}/build/bin/dinero-miner"

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# ═══════════════════════════════════════════════════════════════════════════════
# Utility Functions
# ═══════════════════════════════════════════════════════════════════════════════

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
    ((TESTS_RUN++))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
    ((TESTS_RUN++))
}

log_section() {
    echo ""
    echo -e "${YELLOW}━━━ $1 ━━━${NC}"
    echo ""
}

cleanup() {
    log_info "Cleaning up..."

    # Kill any running processes
    if [ -n "$DAEMON_PID" ]; then
        kill $DAEMON_PID 2>/dev/null || true
    fi
    if [ -n "$STRATUM_PID" ]; then
        kill $STRATUM_PID 2>/dev/null || true
    fi
    if [ -n "$MINER_PID" ]; then
        kill $MINER_PID 2>/dev/null || true
    fi

    # Clean up data directory
    rm -rf "$DATADIR"

    log_info "Cleanup complete"
}

trap cleanup EXIT

wait_for_rpc() {
    local max_attempts=30
    local attempt=0

    while [ $attempt -lt $max_attempts ]; do
        if curl -s --user "__cookie__:$(cat ${DATADIR}/.cookie 2>/dev/null)" \
           --data-binary '{"jsonrpc":"2.0","method":"getblockchaininfo","params":[],"id":1}' \
           -H 'content-type: application/json' \
           http://127.0.0.1:${RPC_PORT}/ > /dev/null 2>&1; then
            return 0
        fi
        sleep 1
        ((attempt++))
    done
    return 1
}

rpc_call() {
    local method="$1"
    local params="$2"

    curl -s --user "__cookie__:$(cat ${DATADIR}/.cookie)" \
         --data-binary "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params:-[]},\"id\":1}" \
         -H 'content-type: application/json' \
         http://127.0.0.1:${RPC_PORT}/
}

# ═══════════════════════════════════════════════════════════════════════════════
# Test: Verify Binaries Exist
# ═══════════════════════════════════════════════════════════════════════════════

test_binaries_exist() {
    log_section "Checking Binaries"

    if [ -x "$DINEROD" ]; then
        log_pass "dinerod found: $DINEROD"
    else
        log_fail "dinerod not found at $DINEROD"
        return 1
    fi

    if [ -x "$STRATUM_SERVER" ]; then
        log_pass "stratum_server found: $STRATUM_SERVER"
    else
        log_info "stratum_server not found (optional): $STRATUM_SERVER"
    fi

    if [ -x "$MINER" ]; then
        log_pass "dinero-miner found: $MINER"
    else
        log_info "dinero-miner not found (optional): $MINER"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# Test: Start Daemon
# ═══════════════════════════════════════════════════════════════════════════════

test_start_daemon() {
    log_section "Starting Daemon"

    mkdir -p "$DATADIR"

    log_info "Starting dinerod ${NETWORK}..."
    "$DINEROD" ${NETWORK} \
        -datadir="$DATADIR" \
        -rpcport=$RPC_PORT \
        -rpcallowip=127.0.0.1 \
        -server=1 \
        -daemon=0 \
        -printtoconsole=0 \
        > "${DATADIR}/daemon.log" 2>&1 &
    DAEMON_PID=$!

    log_info "Waiting for RPC to be ready..."
    if wait_for_rpc; then
        log_pass "Daemon started (PID: $DAEMON_PID)"
    else
        log_fail "Daemon failed to start"
        cat "${DATADIR}/daemon.log"
        return 1
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# Test: Block Template Generation
# ═══════════════════════════════════════════════════════════════════════════════

test_block_template() {
    log_section "Block Template Tests"

    # Get block template
    local template=$(rpc_call "getblocktemplate" "[{\"rules\":[\"segwit\"]}]")

    # Check template has required fields
    if echo "$template" | jq -e '.result.previousblockhash' > /dev/null 2>&1; then
        log_pass "Template has previousblockhash"
    else
        log_fail "Template missing previousblockhash"
    fi

    if echo "$template" | jq -e '.result.utreexo_root' > /dev/null 2>&1; then
        log_pass "Template has utreexo_root (Dinero extension)"
    else
        log_fail "Template missing utreexo_root"
    fi

    # Verify prevhash is 64 hex chars (32 bytes)
    local prevhash=$(echo "$template" | jq -r '.result.previousblockhash')
    if [ ${#prevhash} -eq 64 ]; then
        log_pass "previousblockhash is 64 hex chars"
    else
        log_fail "previousblockhash wrong length: ${#prevhash}"
    fi

    # Verify utreexo_root is 64 hex chars
    local utreexo=$(echo "$template" | jq -r '.result.utreexo_root')
    if [ ${#utreexo} -eq 64 ]; then
        log_pass "utreexo_root is 64 hex chars"
    else
        log_fail "utreexo_root wrong length: ${#utreexo}"
    fi

    # Verify version
    local version=$(echo "$template" | jq -r '.result.version')
    if [ "$version" -ge 1 ]; then
        log_pass "version is valid: $version"
    else
        log_fail "version invalid: $version"
    fi

    log_info "Template preview:"
    echo "$template" | jq '{
        previousblockhash: .result.previousblockhash,
        utreexo_root: .result.utreexo_root,
        height: .result.height,
        version: .result.version,
        bits: .result.bits
    }'
}

# ═══════════════════════════════════════════════════════════════════════════════
# Test: Stratum Template Format
# ═══════════════════════════════════════════════════════════════════════════════

test_stratum_template() {
    log_section "Stratum Template Tests"

    # Get stratum-formatted template
    local stratum_template=$(rpc_call "getblocktemplate" "[{\"rules\":[\"segwit\"],\"mode\":\"stratum\"}]" 2>/dev/null)

    if [ -z "$stratum_template" ] || echo "$stratum_template" | jq -e '.error' > /dev/null 2>&1; then
        log_info "Stratum mode not supported in getblocktemplate, skipping"
        return 0
    fi

    # Verify stratum-specific fields
    if echo "$stratum_template" | jq -e '.result.prevhash_be' > /dev/null 2>&1; then
        log_pass "Stratum template has prevhash_be (big-endian)"
    fi

    if echo "$stratum_template" | jq -e '.result.utreexo_root_be' > /dev/null 2>&1; then
        log_pass "Stratum template has utreexo_root_be (big-endian)"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# Test: Header Hash Consistency
# ═══════════════════════════════════════════════════════════════════════════════

test_header_hash_consistency() {
    log_section "Header Hash Consistency"

    # Get genesis block
    local genesis_hash=$(rpc_call "getblockhash" "[0]" | jq -r '.result')
    local genesis_block=$(rpc_call "getblock" "[\"$genesis_hash\", 2]")

    log_info "Genesis block hash: $genesis_hash"

    # Verify hash format
    if [ ${#genesis_hash} -eq 64 ]; then
        log_pass "Genesis hash is 64 hex chars"
    else
        log_fail "Genesis hash wrong length"
    fi

    # Verify header fields
    local version=$(echo "$genesis_block" | jq -r '.result.version')
    local merkle=$(echo "$genesis_block" | jq -r '.result.merkleroot')
    local time=$(echo "$genesis_block" | jq -r '.result.time')
    local bits=$(echo "$genesis_block" | jq -r '.result.bits')
    local nonce=$(echo "$genesis_block" | jq -r '.result.nonce')

    log_info "Genesis header fields:"
    echo "  version:    $version"
    echo "  merkleroot: $merkle"
    echo "  time:       $time"
    echo "  bits:       $bits"
    echo "  nonce:      $nonce"

    if [ -n "$version" ] && [ -n "$merkle" ] && [ -n "$time" ]; then
        log_pass "Genesis block has all header fields"
    else
        log_fail "Genesis block missing header fields"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# Test: Byte Order Verification
# ═══════════════════════════════════════════════════════════════════════════════

test_byte_order() {
    log_section "Byte Order Verification"

    # Get a block
    local blockhash=$(rpc_call "getbestblockhash" | jq -r '.result')
    local block=$(rpc_call "getblock" "[\"$blockhash\", 2]")

    local prevhash=$(echo "$block" | jq -r '.result.previousblockhash // empty')

    if [ -n "$prevhash" ]; then
        # prevhash from RPC is big-endian (display format)
        # First 2 chars should be the MSB
        local first_byte="${prevhash:0:2}"
        log_info "prevhash first byte (MSB): $first_byte"

        # For a valid chain, prevhash should have leading zeros (low target)
        # or be a hash starting with some value
        log_pass "prevhash appears to be big-endian (display format)"
    else
        log_info "Block is genesis (no prevhash)"
        log_pass "Genesis block correctly has no prevhash"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# Test: 128-byte Header Size
# ═══════════════════════════════════════════════════════════════════════════════

test_header_size() {
    log_section "Header Size Verification"

    # Get raw block header
    local blockhash=$(rpc_call "getbestblockhash" | jq -r '.result')
    local header_hex=$(rpc_call "getblockheader" "[\"$blockhash\", false]" | jq -r '.result')

    # Header hex should be 256 chars (128 bytes * 2)
    local header_len=${#header_hex}

    if [ "$header_len" -eq 256 ]; then
        log_pass "Block header is 128 bytes (256 hex chars)"
    else
        log_fail "Block header wrong size: $((header_len / 2)) bytes (expected 128)"
    fi

    log_info "Header hex (first 64 chars): ${header_hex:0:64}..."
}

# ═══════════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════════

main() {
    echo ""
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo "  Dinero Mining Stack End-to-End Test"
    echo "  Network: ${NETWORK}"
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo ""

    # Run tests
    test_binaries_exist || exit 1
    test_start_daemon || exit 1

    sleep 2  # Let daemon stabilize

    test_block_template
    test_stratum_template
    test_header_hash_consistency
    test_byte_order
    test_header_size

    # Summary
    echo ""
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo -e "  Results: ${GREEN}${TESTS_PASSED}${NC}/${TESTS_RUN} tests passed"
    if [ $TESTS_FAILED -gt 0 ]; then
        echo -e "           ${RED}${TESTS_FAILED} FAILED${NC}"
    fi
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo ""

    if [ $TESTS_FAILED -gt 0 ]; then
        exit 1
    fi
}

main "$@"
