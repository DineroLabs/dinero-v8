# Candidate selection after chainwork repair

The PR #724 Linux restart/churn failure (Actions run 34470037432) left one
regtest node at height 94 while its peer reached 98. The lagging node had
headers through 98; activation repeatedly reported zero missing bodies but
selected its existing height-94 tip.

The candidate registries stored block-index pointers in a `std::set` whose
comparator dereferenced mutable chainwork. Header import and index
materialization repair that work in place. Changing a comparator key while
it remains in a set violates the set ordering invariant. Reinsertion is not
a repair: it can leave duplicate membership and stale ordering.

`BlockCandidates` now tracks membership by pointer identity. Selection evaluates
current work with the existing `ByWorkThenHash` comparator. Hash tie-breaking,
branch eligibility, invalidity checks, and operational retry cooldowns retain
their existing rules. Ordered snapshots sort current work when requested.
Both the global block-index registry and ChainstateService use this container.

Selection scans the current candidate tips (O(number of candidates)); membership
operations are average O(1). This avoids maintaining an ordering index over
externally mutable fields. Existing callers retain responsibility for locking.

## Regression evidence

The candidate eligibility test covers repaired work before and after
reinsertion, duplicate prevention, erase after mutation, equal-work hash
tie-breaking, and eligibility filtering. Using the prior ordered-set/first-ready
selection strategy fails the refreshed-work, duplicate-membership, and tie-break
checks. The updated implementation passes the registered candidate-eligibility
and fork-choice tests against the rebuilt libraries.

This reproduces a concrete ordering defect consistent with the captured stall.
The historical intermittent convergence issues require follow-up observations;
a passing soak alone does not establish that every failure had this cause.

## Rebuilt-daemon verification

A Release build of `dinerod`, `test_reorg_candidate_eligibility`, and
`test_fork_choice` completed on macOS arm64. Both registered unit tests passed.
The unchanged `RestartChurnBoringnessGate` then passed in 347.17 seconds,
including CSN restart, balanced and asymmetric mining restart, and mempool
persistence stages. Linux CI remains required; this local pass does not close
the historical intermittent convergence investigation.
