# Covenant mutation coverage

Assurance artifact for release gate 1 of the covenant activation
(`COVENANT_MAINNET_ACTIVATION_100000.md`), specifically the *mutation* evidence.

## What this answers

A green covenant test lane proves the tests pass. It does not prove they would
go **red** if a consensus rule were deleted. Those are different claims, and
only the second one is evidence that the rules are actually enforced.

`tools/covenant_mutation_harness.py` answers the second question mechanically:
it removes one consensus rule at a time from `src/consensus/covenants.cpp`,
rebuilds, runs the covenant lane, and requires the lane to fail. A mutation that
**survives** — production broken, tests still green — is reported as a coverage
gap and fails the run.

## Reproducing from a clean checkout

```sh
cmake -S . -B build
python3 tools/covenant_mutation_harness.py --build-dir build
```

Exit status is 0 only when every mutation is caught. Optional flags:
`--only <id>` to run a single mutation, `--json <path>` to write a machine
readable report.

Runtime is roughly 3 seconds per mutation plus one baseline build.

## Expected result

As of the commit that introduced this document:

```
caught       16
SURVIVED     0
anchor stale 0
unbuildable  0
score        16/16 (100%)
```

The 16 mutations cover the BIP-119 template-hash construction (scriptSig length
prefix, the non-empty-scriptSig condition, input-index binding, output length
prefix, and rejection of Dinero's shielded / explicit-fee / confidential
extensions) and all the CCV successor-binding rules (counter increment, terminal
counter, code immutability, code identity, state-hash recomputation, value
preservation, transparent spent and successor outputs, control-block parity, the
state size bound, and successor uniqueness).

## A gap this actually found

> **This was a test-coverage gap, not a consensus vulnerability.** The
> recomputation check is present in production and always has been; it is
> load-bearing and it works. What was missing was a test that would have
> *noticed* if it were ever removed. Nothing below describes a defect in shipped
> behaviour — it describes what the check protects against, which is why its
> removal needed to be detectable and now is.

On its first run the harness reported one survivor: **`ccv_state_hash_recompute`**.
Deleting the check that both state hashes recompute from their contents left the
entire covenant lane green.

It mattered. The P2TR scripts commit only to `stateHash`; the `counter` and
`data` fields are bound to that hash by nothing else. An attacker could therefore
present a `stateHash` that still derives the correct spent and successor scripts
while lying about the counters, and the counter-monotonicity rule would end up
validating attacker-chosen numbers committed to nothing.

Every existing test missed it because they either built internally consistent
states, or corrupted `stateHash` in a way that *also* broke the derived scripts —
so the transition failed on script binding rather than on the recompute rule.

Closed by `RejectsCounterNotCommittedByTheStateHash` and
`RejectsDataNotCommittedByTheStateHash` in `tests/consensus/test_ccv_adversarial.cpp`.
Both deliberately skip `RebuildOutputs()`, which is exactly what makes them bite.

## Safety

The harness edits production source by construction. Restoration is treated as
its primary correctness property:

- original bytes and their SHA-256 are captured before any edit;
- restoration runs in a `finally` block — on success, failure, exception, or
  interrupt;
- the digest is re-verified afterwards and the run aborts loudly on mismatch.

If it ever aborts with a digest mismatch, restore `src/consensus/covenants.cpp`
from git before doing anything else. Never commit while the harness is mid-run.

## Deliberate limits

- It mutates `covenants.cpp` only. Covenant rules enforced elsewhere — activation
  heights in `chainparams_impl.cpp`, relay policy, mempool re-validation — are
  covered by their own tests and are not scored here.
- The mutation catalogue is hand-written, not generated. It scores the rules
  someone thought to list; it cannot report a rule nobody wrote down.
- A stale anchor (a mutation whose target text no longer appears exactly once)
  fails the run rather than being skipped silently, so coverage cannot quietly
  shrink while still reporting 100%.
