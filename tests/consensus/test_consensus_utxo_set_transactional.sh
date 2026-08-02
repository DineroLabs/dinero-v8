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
# WHAT THIS DELIBERATELY DOES NOT PIN
# -----------------------------------
# It does NOT require the fuzzer to exit 0 on these seeds, and that is not an
# oversight.
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

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }

[[ -x "${FUZZER}" ]] || fail "consensus_fuzzer not built at ${FUZZER}"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

info "pinning ${#SEEDS[@]} seeds against the #490 undo-delta corruption"

failures=0
for seed in "${SEEDS[@]}"; do
    log="${WORK_DIR}/seed_${seed}.log"
    # The fuzzer may still exit non-zero on the separate accumulator defect;
    # that is expected and is not what this test measures.
    "${FUZZER}" "${seed}" "${ITERATIONS}" >"${log}" 2>&1 || true

    hits="$(grep -c "${CORRUPTION_MARKER}" "${log}" || true)"
    if [[ "${hits}" -ne 0 ]]; then
        printf '[FAIL] seed %s: undo delta claimed %s deletion(s) that never happened\n' \
            "${seed}" "${hits}" >&2
        grep -n "${CORRUPTION_MARKER}" "${log}" | head -5 >&2
        failures=$((failures + 1))
    else
        pass "seed ${seed}: no corrupt deletion records"
    fi

    # Anti-vacuity: a run that never reached the accumulator would trivially
    # show zero corruption markers. Require evidence the seed actually
    # exercised block application.
    if ! grep -q "Blocks applied:" "${log}"; then
        fail "seed ${seed}: fuzzer produced no block-application summary; the run did not exercise the code under test"
    fi
done

[[ "${failures}" -eq 0 ]] || fail "${failures} seed(s) still record deletions that never happened"

pass "all ${#SEEDS[@]} pinned seeds free of undo-delta corruption"
info "Not asserted here: fuzzer exit status. These seeds can still fail on the"
info "separate accumulator proof/root defect (utreexo_accumulator.cpp:1302),"
info "which is a coordinated consensus change and out of scope. Tighten this"
info "test to require exit 0 once that is fixed."
exit 0
