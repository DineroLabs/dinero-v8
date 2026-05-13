#!/usr/bin/env bash
#
# Reproduce a known baseline crash from deterministic acceptance replay.
# This is a standalone regression harness and MUST NOT be used as an RC parity gate.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

ACCEPTANCE_SCRIPT="${ACCEPTANCE_SCRIPT:-${SCRIPT_DIR}/test_acceptance_parity.sh}"
BASELINE_BIN="${BASELINE_DINEROD:-}"
CURRENT_BIN="${CURRENT_DINEROD:-${ROOT_DIR}/build/dinerod}"

CRASH_INDEX="${CRASH_INDEX:-}"
if [[ -n "${CRASH_INDEX}" ]]; then
    BLOCK_COUNT="${BLOCK_COUNT:-$((CRASH_INDEX + 40))}"
else
    BLOCK_COUNT="${BLOCK_COUNT:-220}"
fi
CORPUS_SEED="${CORPUS_SEED:-deterministic-burst-v1}"
STRICT_INVALID_FIXTURES="${STRICT_INVALID_FIXTURES:-0}"
BASELINE_LABEL="${BASELINE_LABEL:-crash-repro}"
BASELINE_SHA="${BASELINE_SHA:-unknown}"

WORKDIR="${WORKDIR:-/tmp/din_baseline_crash_repro_$$}"
LOG_FILE="${WORKDIR}/acceptance.log"
PARITY_WORKDIR="${WORKDIR}/parity"

usage() {
    cat <<USAGE
Usage: tests/test_baseline_crash_repro.sh

Environment:
  BASELINE_DINEROD=<path>             Baseline dinerod binary (required)
  CURRENT_DINEROD=<path>              Current dinerod binary (default: ${ROOT_DIR}/build/dinerod)
  CRASH_INDEX=<n>                     Optional expected baseline crash index (exact match when set)
  BLOCK_COUNT=<n>                     Replay corpus size (default: CRASH_INDEX + 40, else 220)
  CORPUS_SEED=<seed>                  Corpus seed (default: deterministic-burst-v1)
  BASELINE_LABEL=<tag>                Baseline label for provenance (default: crash-repro)
  BASELINE_SHA=<sha>                  Baseline SHA for provenance (default: unknown)
  STRICT_INVALID_FIXTURES=0|1         Forwarded to acceptance harness (default: 0)
  WORKDIR=<path>                      Output directory (default: /tmp/din_baseline_crash_repro_<pid>)

Note:
  This harness is intentionally separate from RC parity.
  RC parity must compare two healthy binaries.
USAGE
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

if [[ ! -x "${ACCEPTANCE_SCRIPT}" ]]; then
    echo "ERROR: acceptance script is not executable: ${ACCEPTANCE_SCRIPT}" >&2
    exit 2
fi
if [[ -z "${BASELINE_BIN}" || ! -x "${BASELINE_BIN}" ]]; then
    echo "ERROR: BASELINE_DINEROD must be set to an executable binary" >&2
    exit 2
fi
if [[ ! -x "${CURRENT_BIN}" ]]; then
    echo "ERROR: CURRENT_DINEROD is not executable: ${CURRENT_BIN}" >&2
    exit 2
fi
if [[ -n "${CRASH_INDEX}" ]] && (( BLOCK_COUNT < CRASH_INDEX )); then
    echo "ERROR: BLOCK_COUNT (${BLOCK_COUNT}) must be >= CRASH_INDEX (${CRASH_INDEX})" >&2
    exit 2
fi

mkdir -p "${WORKDIR}"

extract_summary_value() {
    local key="$1"
    awk -F= -v k="${key}" '$1 == k { print substr($0, length($1) + 2) }' "${LOG_FILE}" | tail -n 1
}

echo "[baseline-crash-repro] running acceptance replay..."
set +e
env \
    BASELINE_DINEROD="${BASELINE_BIN}" \
    CURRENT_DINEROD="${CURRENT_BIN}" \
    BASELINE_LABEL="${BASELINE_LABEL}" \
    BASELINE_SHA="${BASELINE_SHA}" \
    BLOCK_COUNT="${BLOCK_COUNT}" \
    CORPUS_SEED="${CORPUS_SEED}" \
    STRICT_INVALID_FIXTURES="${STRICT_INVALID_FIXTURES}" \
    WORKDIR="${PARITY_WORKDIR}" \
    "${ACCEPTANCE_SCRIPT}" --require-baseline > "${LOG_FILE}" 2>&1
PARITY_RC=$?
set -e

BASELINE_RPCERR_INDEX="$(extract_summary_value BASELINE_RPCERR_INDEX)"
BASELINE_CRASH_FIXTURE="$(extract_summary_value BASELINE_CRASH_FIXTURE)"
VALID_ACCEPTED_CURRENT="$(extract_summary_value VALID_ACCEPTED_CURRENT)"
FIRST_VALID_DIVERGENCE="$(extract_summary_value FIRST_VALID_DIVERGENCE)"

echo "[baseline-crash-repro] parity_rc=${PARITY_RC}"
echo "[baseline-crash-repro] baseline_rpcerr_index=${BASELINE_RPCERR_INDEX:-unset}"
if [[ -n "${CRASH_INDEX}" ]]; then
    echo "[baseline-crash-repro] expected_crash_index=${CRASH_INDEX}"
fi
echo "[baseline-crash-repro] current_accepted=${VALID_ACCEPTED_CURRENT:-unset}/${BLOCK_COUNT}"
echo "[baseline-crash-repro] first_divergence=${FIRST_VALID_DIVERGENCE:-unset}"
echo "[baseline-crash-repro] crash_fixture=${BASELINE_CRASH_FIXTURE:-unset}"
echo "[baseline-crash-repro] log=${LOG_FILE}"

if [[ -z "${BASELINE_RPCERR_INDEX}" || "${BASELINE_RPCERR_INDEX}" == "0" ]]; then
    echo "FAIL: baseline crash was not reproduced (no BASELINE_RPCERR_INDEX reported)" >&2
    tail -n 80 "${LOG_FILE}" >&2 || true
    exit 1
fi

if [[ -n "${CRASH_INDEX}" ]]; then
    if [[ "${BASELINE_RPCERR_INDEX}" != "${CRASH_INDEX}" ]]; then
        echo "FAIL: baseline crash index mismatch (expected ${CRASH_INDEX}, got ${BASELINE_RPCERR_INDEX})" >&2
        tail -n 80 "${LOG_FILE}" >&2 || true
        exit 1
    fi
fi

if [[ "${VALID_ACCEPTED_CURRENT}" != "${BLOCK_COUNT}" ]]; then
    echo "FAIL: current binary did not accept full corpus (${VALID_ACCEPTED_CURRENT}/${BLOCK_COUNT})" >&2
    tail -n 80 "${LOG_FILE}" >&2 || true
    exit 1
fi

if [[ ! -f "${BASELINE_CRASH_FIXTURE}" ]]; then
    echo "FAIL: crash fixture file not found: ${BASELINE_CRASH_FIXTURE}" >&2
    tail -n 80 "${LOG_FILE}" >&2 || true
    exit 1
fi

echo "PASS: baseline crash reproduced deterministically at index ${BASELINE_RPCERR_INDEX}"
exit 0
