# Candidate coin capture for Orchard branch replay

## Problem and behavior

Branch replay previously passed its complete parent coin map directly to mixed transaction validation. `OrchardCandidateCoinView` now captures the external inputs and the new output outpoints for one exact candidate. `OrchardBranchReplay::Append` uses this captured view before recording the candidate's transaction IDs, then invokes the existing shared coin/script/Orchard validator.

Capture requires an independently authenticated parent coin source, complete historical and current-branch transaction membership, and the exact parent stump. The caller must keep those sources stable throughout capture. The view does not establish the provenance of an arbitrary supplied source.

The capture checks:

- Parent height, header identity, reserved bytes, accumulator commitment, candidate identity, size and active context.
- Every transaction's absence from the complete parent transaction inventory; a checked parent-coin lookup must also confirm that every new output is absent. Storage errors are not absence.
- Ordered inputs from historical and Orchard transaction families, duplicate inputs/transactions, and same-block creation before spending.
- Exact input metadata, including creation height and coinbase status. Legacy accumulator leaves do not authenticate these two fields, so they must match the independent parent authority. Same-block metadata must match the actual earlier output.
- Exact external leaf membership, unique in-range positions, expected proof format, complete metadata consumption and proof framing.

Only external input coins and proven candidate-output absences remain in the captured parent view. Unknown queries return an unavailable status, rather than an invented `NotFound`. Shared validation resolves same-block inputs through its existing overlay and still performs signatures, Orchard authorizations, locks, maturity, amount and reward checks.

Capture does not write the database, update the forest, publish a tip, or enable compact-node admission. A refused branch append retains the existing poisoned-owner behavior.

## Scope and remaining work

The production branch caller still owns the full replayed coin map and forest. This change does not make the entire branch replay compact. It does not provide a durable legacy-metadata catalog, an authenticated complete transaction catalog for compact-node restart, or a roots-only canonical writer.

Compact-node support still requires those durable owners, an atomic canonical transition including undo and all companion state, startup and reindex recovery, and actual compact-node admission and lifecycle qualification. Existing refusal guards remain in place. Mainnet activation remains unset.

## Local qualification

Fresh ON and OFF configurations built `dinerod` and the declared replay target. Seven selected CTests passed: the new candidate component and existing forest transition, four ON replay registrations containing 13 actual cases, and the OFF replay registration containing 10 actual cases.

The new component has five groups covering real Orchard authorization plus a signed transparent child, independent captured-view lifetime, exact legacy and same-block metadata, proof corruption/framing, source errors, contradictory output inventory, transaction ordering, context binding and empty external proofs. Its parent authority is a synthetic fixture; it is not an independent historical-provenance qualification.

All 35 project C++ translation units linked into the component were freshly built with ASan/UBSan. Three copied data-check omission controls each failed the intended refusal assertion; the restored component passed all five groups. Separately, the actual branch-replay binary was instrumented with all 319 linked project C++ translation units plus the bundled Bech32 translation unit; all 13 selected replay cases passed with their existing deadlines.

Other external libraries, Rust, C and PQClean were not instrumented; macOS leak detection was off. The full daemon and OFF binaries were normally built and tested, not covered by those sanitizer graphs. No current Linux, whole-node compact mode, platform, power-loss, or release-readiness claim follows from these results.

The initial fixture compile failure, component refusal before the fixture-header initialization correction, build-script dependency-path error, and replay instrumentation mapping error were preserved with their exact unsuccessful results. The implementation and new tests were introduced together; no original-source failing-test claim is made. No production guard, existing assertion or test deadline was relaxed.
