#!/usr/bin/env bash
#
# Regression pin for issue #490: ConsensusUTXOSet must never record a Utreexo
# deletion that did not happen.
#
# WHAT THIS PINS
# --------------
# ConsensusUTXOSet::ApplyBlock previously wrote the deletion into the undo delta
# BEFORE attempting removal, and discarded remove()'s return value. A failed
# removal therefore left the delta claiming "position P was deleted" when P had
# never been deleted. UndoBlock later replayed that into restoreDeletedLeaf(P),
# which reported:
#
#     [Utreexo Restore] Position P was not deleted
#
# ConsensusFuzzer reproduced it on 5 of 40 fixed seeds. Those five are pinned
# here. Each is deterministic; the failure only looked intermittent because the
# fuzzer's default seed is time-based.
#
# EXIT STATUS MUST BE ZERO
# ------------------------
# An earlier draft swallowed exit status with `|| true`; a later one tolerated
# nonzero under one named signature. Neither is needed now -- see step 4 below.
#
# The forbidden-signature check remains: the lossy Snapshot()/Restore()
# symptoms this repair eliminated must not reappear, and the scan result is
# reported affirmatively rather than as an absence.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FUZZER="${CONSENSUS_FUZZER:-${ROOT_DIR}/build/tests/consensus/consensus_fuzzer}"
ITERATIONS="${ITERATIONS:-1000}"
# The FULL deterministic sweep, not only the five seeds that originally failed.
#
# Pinning just the known-bad seeds would be a weaker gate: a regression that
# broke a different seed would pass unnoticed. A complete 1..40 sweep costs a
# few seconds and covers the space the original 5/40 failure was found in.
#
# Recorded result with trusted-position removal: 40/40 exit 0, 0 corruption
# markers. Before it (proof-based removal): seeds 6, 25, 27, 31, 33 exit 1.
SEEDS=($(seq 1 40))
KNOWN_PREVIOUSLY_FAILING="6 25 27 31 33"
# Emitted by UtreexoForest::restoreDeletedLeaf when asked to restore a position
# that was never deleted -- i.e. when the undo delta lied.
CORRUPTION_MARKER="was not deleted"
# Symptoms of the LOSSY Snapshot()/Restore() rollback that this repair removed.
# They must not reappear. Tolerating them would let the lossy path creep back in
# unnoticed -- an earlier draft of this script did exactly that.
FORBIDDEN_SIGNATURES=(
    "Serialized roots do not match node/deletion state"
    "forest deserialize refused payload"
    "Forest root mismatch after restore"
)

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }

[[ -x "${FUZZER}" ]] || fail "consensus_fuzzer not built at ${FUZZER}"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

info "pinning ${#SEEDS[@]} seeds (full 1..40 sweep) against the #490 undo-delta corruption"
info "previously failing under proof-based removal: ${KNOWN_PREVIOUSLY_FAILING}"

failures=0
forbidden_hits=0
forbidden_scans=0
for seed in "${SEEDS[@]}"; do
    log="${WORK_DIR}/seed_${seed}.log"
    status=0
    "${FUZZER}" "${seed}" "${ITERATIONS}" >"${log}" 2>&1 || status=$?

    # 1. A crash or abort is never acceptable, whatever the log says.
    if [[ "${status}" -ge 128 ]]; then
        printf '[FAIL] seed %s: fuzzer terminated by signal (exit %s)\n' "${seed}" "${status}" >&2
        tail -30 "${log}" >&2
        failures=$((failures + 1))
        continue
    fi

    # 2. The property under repair: no fabricated deletion records.
    hits="$(grep -c "${CORRUPTION_MARKER}" "${log}" || true)"
    if [[ "${hits}" -ne 0 ]]; then
        printf '[FAIL] seed %s: undo delta claimed %s deletion(s) that never happened\n' \
            "${seed}" "${hits}" >&2
        grep -n "${CORRUPTION_MARKER}" "${log}" | head -5 >&2
        failures=$((failures + 1))
        continue
    fi

    # 2b. The lossy-restore path this repair eliminated must stay eliminated.
    #
    # The count is tallied and reported affirmatively at the end. Reporting only
    # an absence would be unfalsifiable: a scan that never ran also produces no
    # hits, and ctest suppresses a passing test's output, so "no forbidden
    # strings in the CI log" proves nothing on its own.
    for forbidden in "${FORBIDDEN_SIGNATURES[@]}"; do
        forbidden_scans=$((forbidden_scans + 1))
        if grep -q "${forbidden}" "${log}"; then
            forbidden_hits=$((forbidden_hits + 1))
            printf '[FAIL] seed %s: lossy Snapshot()/Restore() symptom reappeared: %s\n' \
                "${seed}" "${forbidden}" >&2
            grep -n "${forbidden}" "${log}" | head -3 >&2
            failures=$((failures + 1))
            continue 2
        fi
    done

    # 3. Anti-vacuity: a run that never applied a block shows zero corruption
    #    markers trivially.
    if ! grep -q "Blocks applied:" "${log}"; then
        printf '[FAIL] seed %s: no block-application summary; the run never exercised the code under test\n' \
            "${seed}" >&2
        failures=$((failures + 1))
        continue
    fi

    # 4. Exit status must be ZERO. There is no tolerance branch.
    #
    #    Earlier drafts first swallowed the status with `|| true`, then
    #    tolerated nonzero under one named signature, because the seeds still
    #    tripped the accumulator proof defect. Neither is needed now:
    #    ConsensusUTXOSet uses removeAtKnownPosition() -- the same trusted
    #    primitive the live BlockValidator uses -- so it no longer routes
    #    through the broken proof path, and all 40 pinned seeds exit 0.
    if [[ "${status}" -ne 0 ]]; then
        printf '[FAIL] seed %s: exit %s (exit 0 is required)\n' "${seed}" "${status}" >&2
        tail -40 "${log}" >&2
        failures=$((failures + 1))
        continue
    else
        pass "seed ${seed}: no corrupt deletion records (exit 0)"
    fi
done

# Affirmative, deterministic evidence that the forbidden-signature scan actually
# ran and what it found. A reviewer can read these two numbers instead of
# trusting that an absence means anything.
info "forbidden lossy-restore signature scans: ${forbidden_scans}"
info "forbidden lossy-restore signatures: ${forbidden_hits}"
if [[ "${forbidden_scans}" -eq 0 ]]; then
    fail "forbidden-signature scan never ran; its result is meaningless"
fi

[[ "${failures}" -eq 0 ]] || fail "${failures} seed(s) failed (corruption, crash, vacuous run, forbidden signature, or an unexplained nonzero exit)"

pass "all ${#SEEDS[@]} seeds (1..40) exit 0 and are free of undo-delta corruption"
info "All seeds exit 0. ConsensusUTXOSet uses removeAtKnownPosition(), the same"
info "trusted primitive the live BlockValidator uses, so it no longer routes"
info "through the broken proof path. The underlying prove()/verify() and"
info "serialization defect is NOT fixed by this and remains tracked in #490."
exit 0
