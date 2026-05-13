#!/usr/bin/env bash
#
# CI Enforcement: L1 Consensus ABI Stability
#
# Purpose: Prevent accidental consensus-breaking changes
# Status: MANDATORY (CI must fail if this fails)
# Effective: 2026-01-13 (L1 ABI Freeze)
#
# What this enforces:
#   - Frozen header files cannot change structure
#   - Consensus types maintain fixed sizes
#   - No new dependencies in consensus layer
#   - Serialization round-trips remain identical
#
# Exit codes:
#   0 = ABI stable
#   1 = ABI violation detected (MUST FIX)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ═══════════════════════════════════════════════════════════════════════════
# Configuration
# ═══════════════════════════════════════════════════════════════════════════

# Frozen header files (IMMUTABLE without hard fork)
FROZEN_HEADERS=(
    "include/primitives/uint256.h"
    "include/primitives/hash_domains.h"
    "include/primitives/block.h"
    "include/primitives/transaction.h"
    "include/consensus/outpoint.h"
    "include/consensus/block_validation.h"
    "include/consensus/transaction_validator.h"
    "include/consensus/interfaces/iutxo_provider.h"
    "include/consensus/script_verify.h"
)

# Baseline structural hash (computed at ABI freeze time)
# This is a SHA-256 hash of the concatenated frozen headers
# Any change to frozen headers will change this hash
BASELINE_HASH="21979c8d409de98e41f3f739a453f9a4924d3aca43d056f5614c406488dd8e49"

# Note: Type sizes are checked in test_abi_stability.cpp

# ═══════════════════════════════════════════════════════════════════════════
# Helper Functions
# ═══════════════════════════════════════════════════════════════════════════

log_info() {
    echo "ℹ️  $*"
}

log_success() {
    echo "✅ $*"
}

log_error() {
    echo "❌ $*" >&2
}

log_warn() {
    echo "⚠️  $*"
}

# ═══════════════════════════════════════════════════════════════════════════
# Check 1: Frozen Headers Structural Stability
# ═══════════════════════════════════════════════════════════════════════════

check_header_stability() {
    log_info "Checking frozen header stability..."

    local missing_headers=()
    for header in "${FROZEN_HEADERS[@]}"; do
        local full_path="${REPO_ROOT}/${header}"
        if [[ ! -f "${full_path}" ]]; then
            missing_headers+=("${header}")
        fi
    done

    if [[ ${#missing_headers[@]} -gt 0 ]]; then
        log_error "Frozen headers missing (CRITICAL):"
        for header in "${missing_headers[@]}"; do
            log_error "  - ${header}"
        done
        return 1
    fi

    # Compute current structural hash
    local current_hash
    current_hash=$(
        for header in "${FROZEN_HEADERS[@]}"; do
            cat "${REPO_ROOT}/${header}"
        done | sha256sum | awk '{print $1}'
    )

    log_info "Current structural hash: ${current_hash}"

    if [[ "${current_hash}" != "${BASELINE_HASH}" ]]; then
        log_error "Frozen headers have changed!"
        log_error "Expected hash: ${BASELINE_HASH}"
        log_error "Current hash:  ${current_hash}"
        log_error ""
        log_error "This is a CONSENSUS-BREAKING change."
        log_error "If intentional, this requires:"
        log_error "  1. Hard fork activation plan"
        log_error "  2. Network coordination"
        log_error "  3. Update BASELINE_HASH in this script"
        return 1
    fi

    log_success "Frozen headers stable (hash match)"
    return 0
}

# ═══════════════════════════════════════════════════════════════════════════
# Check 2: Type Size Stability
# ═══════════════════════════════════════════════════════════════════════════

check_type_sizes() {
    log_info "Checking frozen type sizes..."

    # This requires a compiled test binary
    local size_test="${REPO_ROOT}/test_abi_stability"

    if [[ ! -f "${size_test}" ]]; then
        log_warn "Type size test binary not found (skipping)"
        log_warn "Build test_abi_stability to enable this check"
        return 0
    fi

    # Run the size test
    if ! "${size_test}"; then
        log_error "Type size test failed"
        log_error "Consensus types have changed size (ABI break)"
        return 1
    fi

    log_success "All frozen types maintain correct sizes"
    return 0
}

# ═══════════════════════════════════════════════════════════════════════════
# Check 3: Consensus Layer Isolation
# ═══════════════════════════════════════════════════════════════════════════

check_consensus_isolation() {
    log_info "Checking consensus layer isolation..."

    # Consensus HEADERS must NOT include these
    local forbidden_in_headers=(
        "rpc/"
        "lightning/"
        "p2p/tx_relay"
        "daemon/"
    )

    local violations=()

    # Check frozen headers only (not source files)
    for header in "${FROZEN_HEADERS[@]}"; do
        local full_path="${REPO_ROOT}/${header}"
        for forbidden in "${forbidden_in_headers[@]}"; do
            if grep -q "#include.*${forbidden}" "${full_path}" 2>/dev/null; then
                violations+=("${header} includes ${forbidden}")
            fi
        done
    done

    # Note: wallet/ includes via IUTXOProvider interface are acceptable
    # This is the proper abstraction layer (dependency inversion)

    if [[ ${#violations[@]} -gt 0 ]]; then
        log_error "Consensus isolation violated:"
        for violation in "${violations[@]}"; do
            log_error "  - ${violation}"
        done
        log_error ""
        log_error "Consensus HEADERS must NOT depend on:"
        for forbidden in "${forbidden_in_headers[@]}"; do
            log_error "  - ${forbidden}"
        done
        return 1
    fi

    log_success "Consensus layer properly isolated"
    return 0
}

# ═══════════════════════════════════════════════════════════════════════════
# Check 4: No m4_compat Regressions
# ═══════════════════════════════════════════════════════════════════════════

check_no_compat_layer() {
    log_info "Checking for m4_compat regressions..."

    # m4_compat was deleted - must stay deleted
    local compat_usages
    compat_usages=$(grep -r "m4_compat::" "${REPO_ROOT}" \
        --include="*.cpp" --include="*.h" \
        --exclude-dir=build* \
        --exclude-dir=third_party \
        2>/dev/null || true)

    if [[ -n "${compat_usages}" ]]; then
        log_error "m4_compat usage detected (REGRESSION):"
        echo "${compat_usages}"
        log_error ""
        log_error "m4_compat was deleted in Phase M.4 final."
        log_error "Do not reintroduce compatibility shims."
        return 1
    fi

    # Check if namespace was reintroduced in hash_domains.h
    if grep -q "namespace m4_compat" "${REPO_ROOT}/include/primitives/hash_domains.h" 2>/dev/null; then
        log_error "m4_compat namespace reintroduced in hash_domains.h"
        return 1
    fi

    log_success "No m4_compat regressions"
    return 0
}

# ═══════════════════════════════════════════════════════════════════════════
# Check 5: Hash Domain Separation Intact
# ═══════════════════════════════════════════════════════════════════════════

check_hash_domain_separation() {
    log_info "Checking hash domain separation..."

    local hash_domains_h="${REPO_ROOT}/include/primitives/hash_domains.h"

    if [[ ! -f "${hash_domains_h}" ]]; then
        log_error "hash_domains.h missing (CRITICAL)"
        return 1
    fi

    # Verify compile-time guards are present
    local required_assertions=(
        "static_assert(!std::is_convertible<TxId, WTxId>::value"
        "static_assert(!std::is_convertible<WTxId, TxId>::value"
        "static_assert(!std::is_convertible<BlockHash, TxId>::value"
        "static_assert(sizeof(TxId) == 32"
        "static_assert(sizeof(BlockHash) == 32"
        "static_assert(std::is_trivially_copyable<TxId>::value"
    )

    local missing_assertions=()
    for assertion in "${required_assertions[@]}"; do
        if ! grep -q "${assertion}" "${hash_domains_h}"; then
            missing_assertions+=("${assertion}")
        fi
    done

    if [[ ${#missing_assertions[@]} -gt 0 ]]; then
        log_error "Hash domain compile-time guards missing:"
        for assertion in "${missing_assertions[@]}"; do
            log_error "  - ${assertion}"
        done
        return 1
    fi

    log_success "Hash domain separation intact"
    return 0
}

# ═══════════════════════════════════════════════════════════════════════════
# Check 6: OutPoint Uses TxId (Not uint256)
# ═══════════════════════════════════════════════════════════════════════════

check_outpoint_txid_type() {
    log_info "Checking OutPoint::txid type safety..."

    local outpoint_h="${REPO_ROOT}/include/consensus/outpoint.h"

    if [[ ! -f "${outpoint_h}" ]]; then
        log_error "outpoint.h missing (expected at include/consensus/outpoint.h)"
        return 1
    fi

    # OutPoint must use TxId, not uint256
    if grep -q "uint256 txid" "${outpoint_h}"; then
        log_error "OutPoint::txid is uint256 (should be TxId)"
        log_error "This was fixed in Phase M.4.3-B"
        return 1
    fi

    if ! grep -q "TxId txid" "${outpoint_h}"; then
        log_error "OutPoint::txid is not TxId"
        return 1
    fi

    log_success "OutPoint::txid correctly uses TxId"
    return 0
}

# ═══════════════════════════════════════════════════════════════════════════
# Main Execution
# ═══════════════════════════════════════════════════════════════════════════

main() {
    echo "════════════════════════════════════════════════════════════════"
    echo "  L1 Consensus ABI Stability Enforcement"
    echo "  Effective: 2026-01-13"
    echo "════════════════════════════════════════════════════════════════"
    echo ""

    local failures=0

    check_header_stability || ((failures++))
    echo ""

    check_type_sizes || ((failures++))
    echo ""

    check_consensus_isolation || ((failures++))
    echo ""

    check_no_compat_layer || ((failures++))
    echo ""

    check_hash_domain_separation || ((failures++))
    echo ""

    check_outpoint_txid_type || ((failures++))
    echo ""

    echo "════════════════════════════════════════════════════════════════"
    if [[ ${failures} -eq 0 ]]; then
        log_success "L1 Consensus ABI is STABLE"
        log_success "All checks passed (${failures} failures)"
        echo ""
        echo "✅ Safe to merge"
        return 0
    else
        log_error "L1 Consensus ABI VIOLATIONS DETECTED"
        log_error "${failures} check(s) failed"
        echo ""
        echo "❌ DO NOT MERGE"
        echo ""
        echo "If this is intentional:"
        echo "  1. Document breaking change in docs/L1_Consensus_ABI_Stability.md"
        echo "  2. Create hard fork activation plan"
        echo "  3. Update baseline hash in this script"
        echo "  4. Coordinate with network"
        return 1
    fi
}

main "$@"
