# Dormant 60-second consensus qualification — 2026-09-17

Status: local qualification candidate; no production activation height, merge,
or deployment. Base main: `7287d77864fa328521847519559b9ed1d7240c16`.

## Implemented policy

- Target 60 seconds after a future activation height; 120-second history stays intact.
- Keep the 100 DIN initial reward and 1,314,000-block halving heights.
- New floor: 0.5 DIN; the extra 0.78125 DIN epoch is retained before it.
- Keep 100-block coinbase maturity, Utreexo leaf/proof formats, shielded anchor
  history, signed height locks, and per-block resource limits.
- All compiled networks default to disabled. Regtest alone accepts an explicit
  activation argument. Consensus/economics RPCs identify the rule's height.

The ASERT transition uses the candidate branch's header at A-1. Three old
arithmetic defects are corrected only after A: compact target normalization,
fractional-power scaling, and overflow before clamping. The full implementation
contract is in [the design](../specs/sixty-second-block-target.md).

## Local evidence

The final targeted CTest run passed **12/12** in 225.51 seconds.
`SixtySecondActivation` took 72.96 seconds; `SixtySecondShieldedLifecycle`
took 150.65 seconds. The remaining cases cover the arithmetic/oracle/model,
difficulty paths, selector snapshots, maturity and subsidy validators.

Platform: macOS ARM64, Release CMake/Ninja, vendored OpenSSL 3.5.7; dedicated
throwaway regtest datadirs. Daemon SHA-256:

```
0fa2410c702d7592abba2d750adc299353b567dea3bfc4b5d5137cbf2daf7da5
```

- Test-first monetary/timing vectors failed on the old behavior before the
  rule was implemented. Additional regressions failed before the encoder,
  fractional polynomial, and boundary-anchor corrections.
- An independent Python arbitrary-precision model checks 6,080 ASERT rows
  against the actual C++ function, across historical and activated eras.
  Both native and forced portable 128-bit backends pass under UBSan.
- Header/full/value-context/mining tests agree on the boundary target, use a
  fork's own boundary, fail closed on missing context, and restore the old
  rule below activation. Selector snapshots retain values rather than pointers.
- Stateful UTXO and stateless maturity helpers reject depth 99 and accept 100
  for coinbases created immediately before, at, and after the boundary.
- Reference stateless and P2P validators reject a one-una tail overclaim;
  independent totals include historical tail payments before late activation.

The daemon tests exercise full-node/CSN root agreement, rollback through A,
restart during rollback, replay, and reindex. A separate two-node lifecycle
shields before A, transfers at A, then unshields and spends the exact transparent
output. It verifies proofs, removal of spent leaves, reorg restoration and
shielded/Utreexo state after reindex and another restart.

Regtest skips difficulty checks. Those daemon tests are state qualification;
the non-regtest C++ context tests and independent arithmetic/model checks cover
ASERT separately. They are not a real loaded-network PoW qualification.

## Timing model and limitations

[The committed receipt](sixty-second-asert-transition-20260917.json) comes from
`python3 scripts/benchmarks/sixty_second_asert_transition.py <asert-probe>`.
Every one of its 80,000 nBits predictions is checked against production C++.
It uses fixed hash power, Poisson arrivals, integer template timestamps and one
template per block; it models no propagation delay or validation workload.

At adequate hash power the last 5,000 blocks average 59.58–59.70 seconds.
Initial 100-block averages range from 39.02 to 280.21 seconds; a clean
steady-state average does not qualify that transient for production.

The low-hash profile remains near 118 seconds because the configured target
limit binds. Its boundary work increases about 2.91x when moving from a legacy
encoded target above the nominal cap to the correctly capped target. Historical
validation allowed those ASERT outputs; the upgrade must not reinterpret them.
Confirm the actual network's hash headroom and independently review this floor
behavior before assigning A. No unconditional continuity or 60-second arrival
claim applies to a binding floor.

## CI coverage and remaining gates

The initial Covenant readiness run
[35186488678](https://github.com/DineroLabs/dinero-v8/actions/runs/35186488678)
passed 17/18 CTests and failed the pinned mainnet configuration checksum in
`CovenantActivation`. The checksum intentionally gained
`sixty_second_activation_height=4294967295` (disabled); the old test vector had
not been updated. The failure was reproduced locally. Independent Python SHA-256
over the literal mainnet parameters reproduced both the old `47202b4f…` digest
and the new `3f288229…` digest, with only that added field.

The corrected test keeps the pinned digest, explicitly checks that mainnet's
activation remains disabled, and verifies that changing the new field changes
the checksum. CovenantActivation, ChainParamsSelection and SixtySecondConsensus
then passed locally (3/3 CTests). This correction changes tests only; the full
Linux Covenant readiness gate still needs to pass on the updated commit.

All five new CTests have execution lanes: the arithmetic, oracle and model in
normal Tests; activation and shielded lifecycle in the serial daemon lane.
The local coverage-map check assigns all five. The full inventory gate is
Linux-specific: this Mac configuration additionally registers
MacOSNestedBundleSigning and omits RelayNatNetnsHarness. Do not waive or edit
those platform differences into the Linux baseline.

Remaining before production activation:

1. Exact-commit full Linux CI and Linux daemon lifecycle qualification.
2. Independent review of the monetary policy, ASERT arithmetic/rounding, A-1
   reference, timestamp manipulation, target floor and transition behavior.
3. PoW-enforced private-network tests, hash-rate changes, partitions/reorgs,
   and sustained worst-case block validation on Linux/ARM and mobile hardware,
   including a CSN validating shielded/unshield transactions across A.
4. Snapshot/import and header-backfill availability of the boundary anchor;
   failure is closed but serving/mining must recover once the header is available.
5. Combined compact-proof activation and wallet/daemon/pool protocol qualification.
6. Release/upgrade coordination, network hash headroom, a concrete activation
   height and a rehearsed contingency plan.

Passing ordinary CI does not close the review, resource, network or rollout gates.
