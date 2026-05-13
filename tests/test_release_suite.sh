#!/usr/bin/env bash
#
# Mainnet release suite meta-gate.
# Runs all major RC checks in one deterministic sequence.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
MACOS_TARGET_FILE="${ROOT_DIR}/.macos-deployment-target"

read_repo_macos_target() {
    if [[ -f "${MACOS_TARGET_FILE}" ]]; then
        local target
        target="$(tr -d '[:space:]' < "${MACOS_TARGET_FILE}")"
        if [[ -n "${target}" ]]; then
            printf '%s\n' "${target}"
            return
        fi
    fi
    printf '%s\n' "13.0"
}

DEFAULT_MACOS_RELEASE_MIN_TARGET="$(read_repo_macos_target)"

RELEASE_REQUIRE_BASELINE="${RELEASE_REQUIRE_BASELINE:-1}"
# Policy: AcceptanceParity runs in regtest mode, where a subset of invalid fixtures
# (e.g., PoW/diffbits family) may be accepted by both binaries. Keep strict mode opt-in.
STRICT_INVALID_FIXTURES="${STRICT_INVALID_FIXTURES:-0}"
IBD_PROFILE="${IBD_PROFILE:-release}"
RELEASE_BUILD_FIRST="${RELEASE_BUILD_FIRST:-1}"
BUILD_JOBS="${BUILD_JOBS:-8}"
RC_MODE="${RC_MODE:-0}"
MACOS_RELEASE_MIN_TARGET="${MACOS_RELEASE_MIN_TARGET:-${DEFAULT_MACOS_RELEASE_MIN_TARGET}}"
BASELINE_LABEL="${BASELINE_LABEL:-unknown}"

WALLET_GATE_FILTER="${WALLET_GATE_FILTER:-WalletMainnetReadiness}"
P2P_STORM_SCRIPT="${P2P_STORM_SCRIPT:-${SCRIPT_DIR}/test_p2p_storm.sh}"
IBD_TORTURE_SCRIPT="${IBD_TORTURE_SCRIPT:-${SCRIPT_DIR}/test_ibd_torture.sh}"
ACCEPTANCE_SCRIPT="${ACCEPTANCE_SCRIPT:-${SCRIPT_DIR}/test_acceptance_parity.sh}"
UTREEXO_SCRIPT="${UTREEXO_SCRIPT:-${SCRIPT_DIR}/test_utreexo_consensus.sh}"
MEMPOOL_SCRIPT="${MEMPOOL_SCRIPT:-${SCRIPT_DIR}/test_mempool_stress.sh}"
RESTART_CHURN_GATE_SCRIPT="${RESTART_CHURN_GATE_SCRIPT:-${ROOT_DIR}/tests/integration/test_restart_churn_boringness.sh}"
RUN_RESTART_CHURN_GATE="${RUN_RESTART_CHURN_GATE:-${RC_MODE}}"
RELEASE_FREEZE_SCRIPT="${RELEASE_FREEZE_SCRIPT:-${ROOT_DIR}/tools/freeze_release_inputs.sh}"
RUN_RELEASE_IDENTITY_FREEZE="${RUN_RELEASE_IDENTITY_FREEZE:-${RC_MODE}}"
RUN_PHASE2_SHIELDED_CANONICAL_RECOVERY="${RUN_PHASE2_SHIELDED_CANONICAL_RECOVERY:-0}"
PHASE1_CANONICAL_RECOVERY_LABEL="${PHASE1_CANONICAL_RECOVERY_LABEL:-phase1-canonical-recovery}"
SHIELDED_CANONICAL_RECOVERY_LABEL="${SHIELDED_CANONICAL_RECOVERY_LABEL:-shielded-canonical-recovery}"
PHASE2_SHIELDED_CANONICAL_RECOVERY_LABEL="${PHASE2_SHIELDED_CANONICAL_RECOVERY_LABEL:-phase2-shielded-canonical-recovery}"

PASSED_STAGES=()
SKIPPED_STAGES=()

print_header() {
    echo -e "\n${CYAN}================================================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}================================================================${NC}"
}

info() { echo -e "${BLUE}INFO:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }
warn() { echo -e "${YELLOW}WARN:${NC} $*"; }
fail() { echo -e "${RED}FAIL:${NC} $*" >&2; exit 1; }

usage() {
    cat <<USAGE
Usage: tests/test_release_suite.sh

Environment:
  BUILD_DIR=<path>                    Build directory (default: ${ROOT_DIR}/build)
  RELEASE_REQUIRE_BASELINE=1|0        Require BASELINE_DINEROD for parity gate (default: 1)
  STRICT_INVALID_FIXTURES=1|0         AcceptanceParity strict invalid fixtures (default: 0 for regtest profile)
  IBD_PROFILE=strict|release|smoke    IBD torture profile (default: release)
  RELEASE_BUILD_FIRST=1|0             Build required binaries first (default: 1)
  BUILD_JOBS=<n>                      Parallel build jobs (default: 8)
  RC_MODE=1|0                         RC lock mode: fail on any skipped gate (default: 0)
  MACOS_RELEASE_MIN_TARGET=<ver>      Minimum macOS target expected for release readiness (default: ${DEFAULT_MACOS_RELEASE_MIN_TARGET})
  BASELINE_DINEROD=<path>             Baseline dinerod binary (required by default)
  BASELINE_LABEL=<tag>                Baseline tag/label (required in RC_MODE=1)
  BASELINE_SHA=<sha>                  Baseline git SHA (optional, printed by parity gate)
  RUN_PHASE2_SHIELDED_CANONICAL_RECOVERY=1|0 Run daemon-level shielded crash/restart gate (default: 0)
  PHASE1_CANONICAL_RECOVERY_LABEL=<l> CTest label for canonical recovery bundle (default: phase1-canonical-recovery)
  SHIELDED_CANONICAL_RECOVERY_LABEL=<l> CTest label for shielded recovery bundle (default: shielded-canonical-recovery)
  PHASE2_SHIELDED_CANONICAL_RECOVERY_LABEL=<l> CTest label for daemon-level shielded recovery bundle (default: phase2-shielded-canonical-recovery)
USAGE
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

for script in \
    "${ACCEPTANCE_SCRIPT}" \
    "${P2P_STORM_SCRIPT}" \
    "${IBD_TORTURE_SCRIPT}" \
    "${UTREEXO_SCRIPT}" \
    "${MEMPOOL_SCRIPT}"; do
    [[ -x "${script}" ]] || fail "required script not executable: ${script}"
done

if [[ "${RUN_RESTART_CHURN_GATE}" == "1" ]]; then
    [[ -x "${RESTART_CHURN_GATE_SCRIPT}" ]] || fail "required restart/churn gate not executable: ${RESTART_CHURN_GATE_SCRIPT}"
fi
if [[ "${RUN_RELEASE_IDENTITY_FREEZE}" == "1" ]]; then
    [[ -x "${RELEASE_FREEZE_SCRIPT}" ]] || fail "required release freeze script not executable: ${RELEASE_FREEZE_SCRIPT}"
fi

[[ -d "${BUILD_DIR}" ]] || fail "build directory not found: ${BUILD_DIR}"
[[ -x "${BUILD_DIR}/dinerod" ]] || fail "missing dinerod binary: ${BUILD_DIR}/dinerod"

if ! command -v ctest >/dev/null 2>&1; then
    fail "ctest not found in PATH"
fi

if [[ "${RELEASE_REQUIRE_BASELINE}" == "1" && -z "${BASELINE_DINEROD:-}" ]]; then
    fail "BASELINE_DINEROD is required when RELEASE_REQUIRE_BASELINE=1"
fi

if [[ -n "${BASELINE_DINEROD:-}" && ! -x "${BASELINE_DINEROD}" ]]; then
    fail "BASELINE_DINEROD is not executable: ${BASELINE_DINEROD}"
fi

if [[ "${RC_MODE}" == "1" && "${RELEASE_REQUIRE_BASELINE}" == "1" ]]; then
    if [[ -z "${BASELINE_LABEL}" || "${BASELINE_LABEL}" == "unknown" ]]; then
        fail "RC_MODE=1 requires BASELINE_LABEL (stable tag/label), not 'unknown'"
    fi
fi

run_stage() {
    local stage_name="$1"
    shift

    print_header "Release Suite Stage: ${stage_name}"
    local start
    start="$(date +%s)"
    "$@"
    local end elapsed
    end="$(date +%s)"
    elapsed="$((end - start))"
    pass "${stage_name} (${elapsed}s)"
    PASSED_STAGES+=("${stage_name}")
}

skip_stage() {
    local stage_name="$1"
    local reason="$2"
    print_header "Release Suite Stage: ${stage_name}"
    warn "SKIP: ${reason}"
    SKIPPED_STAGES+=("${stage_name}")
}

join_by() {
    local sep="$1"
    shift
    local out=""
    local item
    for item in "$@"; do
        if [[ -z "${out}" ]]; then
            out="${item}"
        else
            out="${out}${sep}${item}"
        fi
    done
    echo "${out}"
}

version_ge() {
    local left="$1"
    local right="$2"
    python3 - "$left" "$right" <<'PY'
import sys

def parse(v):
    parts = []
    for token in v.split("."):
        try:
            parts.append(int(token))
        except ValueError:
            parts.append(0)
    return parts

left = parse(sys.argv[1])
right = parse(sys.argv[2])
max_len = max(len(left), len(right))
left.extend([0] * (max_len - len(left)))
right.extend([0] * (max_len - len(right)))
print("1" if left >= right else "0")
PY
}

check_macos_release_target() {
    if [[ "$(uname -s)" != "Darwin" ]]; then
        return 0
    fi

    local current_target
    local cache_file="${BUILD_DIR}/CMakeCache.txt"
    if [[ ! -f "${cache_file}" ]]; then
        warn "Unable to determine CMAKE_OSX_DEPLOYMENT_TARGET: missing ${cache_file}"
        return 0
    fi
    current_target="$(awk -F= '/^CMAKE_OSX_DEPLOYMENT_TARGET:STRING=/{print $2; exit}' "${cache_file}")"
    if [[ -z "${current_target}" ]]; then
        warn "Unable to determine CMAKE_OSX_DEPLOYMENT_TARGET from ${BUILD_DIR}"
        return 0
    fi

    info "macOS deployment target=${current_target} (release minimum=${MACOS_RELEASE_MIN_TARGET})"
    if [[ "$(version_ge "${current_target}" "${MACOS_RELEASE_MIN_TARGET}")" != "1" ]]; then
        if [[ "${RC_MODE}" == "1" ]]; then
            fail "macOS deployment target ${current_target} is below release minimum ${MACOS_RELEASE_MIN_TARGET} in RC_MODE=1"
        fi
        warn "macOS deployment target ${current_target} is below release minimum ${MACOS_RELEASE_MIN_TARGET}; release artifacts may not be portable as expected"
    fi
}

print_header "Dinero Mainnet Release Suite"
info "BUILD_DIR=${BUILD_DIR}"
info "RELEASE_REQUIRE_BASELINE=${RELEASE_REQUIRE_BASELINE}"
info "STRICT_INVALID_FIXTURES=${STRICT_INVALID_FIXTURES}"
info "IBD_PROFILE=${IBD_PROFILE}"
info "RELEASE_BUILD_FIRST=${RELEASE_BUILD_FIRST}"
info "RC_MODE=${RC_MODE}"
info "BASELINE_LABEL=${BASELINE_LABEL}"
info "RUN_RESTART_CHURN_GATE=${RUN_RESTART_CHURN_GATE}"
info "RUN_RELEASE_IDENTITY_FREEZE=${RUN_RELEASE_IDENTITY_FREEZE}"

check_macos_release_target

if [[ "${STRICT_INVALID_FIXTURES}" == "1" ]]; then
    warn "STRICT_INVALID_FIXTURES=1 enabled for regtest parity; use PoW-enforced profile if you require hard invalid-fixture rejection"
fi

if [[ "${RELEASE_BUILD_FIRST}" == "1" ]]; then
    run_stage "BuildPrerequisites" \
        cmake --build "${BUILD_DIR}" --target dinerod test_wallet_mainnet_readiness shielded_pool_test shielded_adversarial_test test_shielded_reindex_equivalence shielded_tip_marker_mutator shielded_tip_marker_probe -j"${BUILD_JOBS}"
fi

run_stage "WalletMainnetReadiness" \
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -R "${WALLET_GATE_FILTER}"

if [[ "${RELEASE_REQUIRE_BASELINE}" == "1" ]]; then
    run_stage "AcceptanceParity" \
        env \
            STRICT_INVALID_FIXTURES="${STRICT_INVALID_FIXTURES}" \
            CURRENT_DINEROD="${BUILD_DIR}/dinerod" \
            BASELINE_DINEROD="${BASELINE_DINEROD}" \
            BASELINE_LABEL="${BASELINE_LABEL}" \
            BASELINE_SHA="${BASELINE_SHA:-unknown}" \
            "${ACCEPTANCE_SCRIPT}" --require-baseline
else
    if [[ -z "${BASELINE_DINEROD:-}" ]]; then
        skip_stage "AcceptanceParity" "BASELINE_DINEROD not set (RELEASE_REQUIRE_BASELINE=0); parity not executed"
    else
        warn "Release parity running without --require-baseline (RELEASE_REQUIRE_BASELINE=0)"
        run_stage "AcceptanceParity" \
            env \
                STRICT_INVALID_FIXTURES="${STRICT_INVALID_FIXTURES}" \
                CURRENT_DINEROD="${BUILD_DIR}/dinerod" \
                BASELINE_DINEROD="${BASELINE_DINEROD}" \
                BASELINE_LABEL="${BASELINE_LABEL}" \
                BASELINE_SHA="${BASELINE_SHA:-unknown}" \
                "${ACCEPTANCE_SCRIPT}"
    fi
fi

run_stage "P2PStorm" \
    env DINEROD="${BUILD_DIR}/dinerod" "${P2P_STORM_SCRIPT}"

run_stage "IBDTorture(${IBD_PROFILE})" \
    env DINEROD="${BUILD_DIR}/dinerod" "${IBD_TORTURE_SCRIPT}" --profile "${IBD_PROFILE}"

run_stage "UtreexoConsensus" \
    env DINEROD="${BUILD_DIR}/dinerod" "${UTREEXO_SCRIPT}"

run_stage "Phase1CanonicalRecovery" \
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -L "${PHASE1_CANONICAL_RECOVERY_LABEL}"

run_stage "ShieldedCanonicalRecovery" \
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -L "${SHIELDED_CANONICAL_RECOVERY_LABEL}"

if [[ "${RUN_PHASE2_SHIELDED_CANONICAL_RECOVERY}" == "1" ]]; then
    run_stage "Phase2ShieldedCanonicalRecovery" \
        ctest --test-dir "${BUILD_DIR}" --output-on-failure -L "${PHASE2_SHIELDED_CANONICAL_RECOVERY_LABEL}"
else
    skip_stage "Phase2ShieldedCanonicalRecovery" "RUN_PHASE2_SHIELDED_CANONICAL_RECOVERY=0"
fi

run_stage "MempoolStress" \
    env DINEROD="${BUILD_DIR}/dinerod" "${MEMPOOL_SCRIPT}"

if [[ "${RUN_RESTART_CHURN_GATE}" == "1" ]]; then
    run_stage "RestartChurnBoringness" \
        env PROFILE=gate DINEROD="${BUILD_DIR}/dinerod" "${RESTART_CHURN_GATE_SCRIPT}"
else
    skip_stage "RestartChurnBoringness" "RUN_RESTART_CHURN_GATE=0"
fi

if [[ "${RUN_RELEASE_IDENTITY_FREEZE}" == "1" ]]; then
    run_stage "ReleaseIdentityFreeze" \
        env BUILD_JOBS="${BUILD_JOBS}" "${RELEASE_FREEZE_SCRIPT}"
else
    skip_stage "ReleaseIdentityFreeze" "RUN_RELEASE_IDENTITY_FREEZE=0"
fi

print_header "Release Suite Complete"
if [[ "${#SKIPPED_STAGES[@]}" -gt 0 ]]; then
    warn "Skipped gates: $(join_by ', ' "${SKIPPED_STAGES[@]}")"
    if [[ "${RC_MODE}" == "1" ]]; then
        fail "RC_MODE=1 requires zero skipped gates"
    fi
fi
pass "Release gates passed (executed=${#PASSED_STAGES[@]} skipped=${#SKIPPED_STAGES[@]})"
