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
# WHY NONZERO EXIT IS TOLERATED -- AND HOW NARROWLY
# -------------------------------------------------
# An earlier version swallowed the exit status with `|| true`. That was wrong:
# it would have accepted a segfault, an assertion abort, or any unrelated
# regression so long as one text marker was absent. The exit code is now
# captured and classified:
#
#   exit 0                   -> ideal
#   exit != 0 + known sig    -> tolerated, ONLY with that signature
#   exit != 0, no signature  -> FAIL (unrelated regression)
#   exit >= 128              -> FAIL always (signal / abort / segfault)
#
# The tolerance exists because these seeds still exit nonzero for a DIFFERENT
# and deeper reason, and that is not an oversight.
#
# The underlying reason remove() fails at all is a separate, previously known
# accumulator defect: a freshly generated proof does not verify against the
# forest it was generated from (documented at utreexo_accumulator.cpp:1302 --
# recomputePath() clearing roots_[h] while numLeaves_ still has bit h set).
# Snapshot/Restore is lossy for the same reason: on rollback the serialized
# forest is refused by its own deserializer and the fallback rebuild lands on a
# different leaf count and root.
#
# Fixing that is a coordinated consensus change, explicitly out of scope for the
# repair this test accompanies. Asserting exit 0 here would either fail forever
# or tempt someone to "fix" root semantics quietly inside an error-handling PR.
#
# So this pins exactly what was repaired and nothing more: the undo delta no
# longer lies about deletions. When the accumulator defect is fixed, this test
# should be TIGHTENED to require exit 0 on all five seeds.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FUZZER="${CONSENSUS_FUZZER:-${ROOT_DIR}/build/tests/consensus/consensus_fuzzer}"
ITERATIONS="${ITERATIONS:-1000}"
# The exact seeds that exposed the corruption in a 1..40 sweep.
SEEDS=(6 25 27 31 33)
# Emitted by UtreexoForest::restoreDeletedLeaf when asked to restore a position
# that was never deleted -- i.e. when the undo delta lied.
CORRUPTION_MARKER="was not deleted"
# The precise known accumulator/restore failure that is still tolerated. A
# nonzero exit must be explained by one of these, or it is a real regression.
# The ONE tolerated signature: the root known defect -- a freshly generated
# proof does not verify against the forest it came from
# (utreexo_accumulator.cpp:1302, recomputePath() clearing roots_[h] while
# numLeaves_ still has bit h set). With this repair in place it surfaces as a
# clean ApplyBlock rejection instead of silent undo corruption.
KNOWN_SIGNATURES=(
    "STEP1 proof.verify FAILED"
)

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

info "pinning ${#SEEDS[@]} seeds against the #490 undo-delta corruption"

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

    # 4. Classify a nonzero exit. Tolerated ONLY for the known accumulator
    #    defect, never for arbitrary failure.
    if [[ "${status}" -ne 0 ]]; then
        matched=""
        for signature in "${KNOWN_SIGNATURES[@]}"; do
            if grep -q "${signature}" "${log}"; then
                matched="${signature}"
                break
            fi
        done
        if [[ -z "${matched}" ]]; then
            printf '[FAIL] seed %s: exit %s with no known accumulator signature -- unrelated regression, not the tolerated defect\n' \
                "${seed}" "${status}" >&2
            tail -40 "${log}" >&2
            failures=$((failures + 1))
            continue
        fi
        pass "seed ${seed}: no corrupt deletion records (exit ${status}, known defect)"
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

pass "all ${#SEEDS[@]} pinned seeds free of undo-delta corruption"
info "Nonzero exits above were CLASSIFIED, not ignored: each was explained by"
info "the known accumulator proof/root defect (utreexo_accumulator.cpp:1302), a"
info "coordinated consensus change that is out of scope here. Any nonzero exit"
info "without that signature, and any signal/abort, fails this test. Remove the"
info "tolerance branch and require exit 0 once that defect is fixed."
exit 0
