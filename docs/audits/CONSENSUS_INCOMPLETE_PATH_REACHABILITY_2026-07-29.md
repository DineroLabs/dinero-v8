# Consensus incomplete-path reachability audit

Date: 2026-07-29
Audited revision: `3dc720b37bd2f506ebcfcc839e04d1e727dd6757` (`dinero-main`)
Scope: explicit `TODO`, `FIXME`, `not implemented`, `unimplemented`, and
`placeholder` markers under `src/consensus` and `include/consensus`, followed
through production call sites. This is a reachability audit, not a complete
consensus-security audit.

## Post-audit status

This report preserves the behavior observed at the audited revision; it is not
a description of current covenant readiness. PRs #465–#467 subsequently made
mainnet and testnet CTV/CCV activation dormant, completed the inherited BIP342
interpreter surface, bound CCV transitions to successor outputs, and added
resource and lifecycle coverage. Current operator status and the remaining
production-activation requirements are maintained in
`docs/consensus/COVENANT_PROTOCOL_STATUS.md`.

The other findings below remain scoped to the audited revision unless a later
change is explicitly linked from their issue or owning design document.

## Executive result

The raw scan returned 37 marker matches in 20 files. Grouping repeated comments
and benign uses produced 14 findings:

| Classification | Count |
|---|---:|
| Live consensus behavior | 2 |
| Live operational/network behavior | 3 |
| Compiled or declared but no production caller | 5 |
| Comment, observability, or intentional placeholder | 4 |

At the audited revision, the highest-priority result was not a traditional
TODO: covenant verification was reachable in normal block validation and active
on mainnet from height 1, while `VerifyContractTransition()` explicitly omitted
binding the successor state to a transaction output. The source comment said
not to activate CCV until that was fixed, but the activation parameters already
did so. The absence of historical CCV outputs limited historical exposure; it
did not make a newly created CCV output safe.

The second live consensus item at the audited revision was a hard rejection of
Tapscripts larger than 252 bytes. This was narrower than the 10,000-byte limit
claimed by the interpreter and by BIP342. Because it was already enforced,
correcting it required consensus-change discipline rather than an ordinary
parser fix.

Several alarming-looking difficulty, median-time, chain-walking, block
conversion, and undo-checksum TODOs are not called by the production chain
path. They are incomplete abandoned/parallel architecture, not current
mainnet behavior. They should be removed or quarantined so they cannot be wired
in accidentally.

Finally, the shielded range-proof TODO is misleading rather than an absent
check. The active verifier decodes the field named `aggregated_range_proof` as
a list of per-commitment Borromean proofs and verifies each proof. True
aggregation remains unimplemented; range enforcement does not.

## Method

1. Enumerate explicit incomplete-path markers in the two consensus trees.
2. Collapse duplicate comments and related adapter stubs into findings.
3. Search all source, headers, and tests for callers and construction sites.
4. Trace live callers from daemon startup, block/transaction validation, or P2P
   service entry points.
5. Distinguish the chain's rule as deployed from what upstream protocols or
   comments say it should be.

Reachability labels used below:

- **Live-consensus**: reachable while deciding whether a transaction or block
  is valid.
- **Live-operational**: reachable in a production daemon but does not directly
  decide ledger validity.
- **Dormant**: compiled or declared, but no production call site was found.
- **Non-defect marker**: documentation debt, observability gap, fail-closed
  sentinel, or an intentional data placeholder.

## Prioritized findings

### C-01 — CCV accepts a transition with no committed successor output

**Priority:** P0 before any CCV use
**Reachability at audited revision:** Live-consensus; configured active on
mainnet at height 1

Evidence:

- `CovenantActivationParams::MAINNET_ACTIVATION_HEIGHT` is `1`.
- Normal transaction and script validation call `IsCovenantActive()` and then
  `ScriptVerifier::VerifyTaproot()`.
- `VerifyTaproot()` executes Tapscript with `SCRIPT_VERIFY_STANDARD`.
- `SCRIPT_VERIFY_STANDARD` includes `SCRIPT_VERIFY_COVENANTS`, including
  `SCRIPT_VERIFY_CHECKCONTRACT`.
- `OP_CHECKCONTRACTVERIFY` calls `VerifyContractTransition()`.
- That function checks counter progression, code-hash equality, and the
  supplied new-state hash, then returns true without proving that any output
  commits to the new state.

Impact: a CCV-locked coin can be spent while presenting a syntactically valid
next state without carrying the state machine into a successor output. This
defeats the core covenant invariant. Transaction-wide value conservation is
handled elsewhere; the missing CCV rule is the binding of the required
successor state and its covenant-controlled value.

Historical qualification: the active-chain scan described below establishes
that no CCV leaf has ever been revealed through the audited tip. Taproot makes
the stronger claim—no output commits to a hidden, unspent CCV leaf—impossible
to prove from chain data alone.

Recommendation:

1. Freeze creation/use operationally until the rule and wire commitment are
   specified.
2. Specify exactly how a successor output commits to code hash, state hash,
   counter, script/control path, and value.
3. Implement adversarial tests first: missing successor, multiple apparent
   successors, wrong output index, altered value, duplicate state, fee edge
   cases, and state bytes that parse ambiguously.
4. Introduce the completed rule with explicit activation semantics. Do not
   silently reinterpret already-active opcode behavior.
5. Independently scan all mainnet outputs and known Taproot leaves before
   selecting the activation/migration plan.

Utreexo does not supply the missing covenant invariant. It proves that an input
UTXO existed in the committed set; it does not prove that the spending
transaction creates the required successor contract output.

### C-02 — Tapleaf CompactSize handling makes 253–10,000-byte scripts invalid

**Priority:** P1 design/activation decision
**Reachability:** Live-consensus from the same Taproot script path

`ScriptVerifier::VerifyTaproot()` encodes the Tapleaf script length only when it
is below 253 and otherwise rejects the spend. The interpreter separately claims
and enforces a 10,000-byte maximum, so the effective limit is 252 bytes.

This is deterministic and therefore not a node-split within the current
implementation. It is, however, a deployed divergence from the advertised
BIP342-shaped behavior. Simply adding CompactSize encodings would make
previously invalid spends valid and therefore changes consensus.

Recommendation: decide whether 252 bytes is the intended Dinero rule. If yes,
document and test it as such. If not, implement canonical CompactSize handling
behind an explicit activation and add boundary vectors for 252, 253, 65,535,
65,536, 10,000, and 10,001 bytes.

### O-01 — Startup “block data available” check never opens block files

**Priority:** P1 operational integrity
**Reachability:** Live-operational at daemon startup

`daemon_app.cpp` runs `StartupValidator::Validate()`, which calls
`CheckBlockDataAvailability()`. The latter inspects recent header disk
positions but never checks that the referenced file exists or that the record
can be read. It can therefore report “Block data is available” for missing or
unreadable data.

Recommendation: resolve each sampled `FilePosition` through `BlockStorage` and
perform at least a bounded record/header read with size and checksum validation.
Test missing file, truncated record, bad checksum, pruned marker, genesis
offset, and healthy startup. Treat this as a startup/recovery fix, not a
consensus-rule change.

### O-02 — Header-sync completion trusts one peer

**Priority:** P2 network hardening
**Reachability:** Live-operational when header sync completes

`HeaderSyncP2P` logs that verification with all outbound peers is a TODO and
does not perform it. This is eclipse-resistance/peer-diversity debt, not proof
that invalid headers bypass local header validation.

Recommendation: compare the selected best chain with multiple eligible
outbound peers, define disagreement and timeout policy, and expose the result
in sync status. Test disagreement, stalled peers, one-peer operation, and peer
replacement.

### O-03 — Header-sync target is self-referential

**Priority:** P3 observability
**Reachability:** Live-operational status reporting

`HeaderSyncManager::GetStatus()` reports `headers_target =
best_header_height_`, so displayed progress can appear complete without a
peer-advertised target. This does not alter validation.

Recommendation: report an optional/unknown target until a validated peer target
exists; never manufacture 100% progress from the local height.

## Dormant incomplete architecture

### D-01 — “Canonical” ASERT implementation truncates arithmetic to 64 bits

**Priority:** P1 removal/quarantine; P0 if anyone proposes wiring it in
**Reachability:** Dormant

`CalculateNextWork_ASERT_Canonical()` calls a placeholder `MultiplyTarget()`
which reduces a 256-bit target to 64 bits. The only caller found is
`test_asert_canonical.cpp`; production difficulty uses the shared candidate
difficulty functions instead.

Recommendation: remove the duplicate implementation and misleading test, or
replace it with a clearly isolated reference implementation and authoritative
vectors. Add a build-time or architectural guard against selecting it as the
production DAA.

### D-02 — `PowConsensusEngine::GetCurrentDifficulty()` returns a constant

**Priority:** P2 interface cleanup; P0 if a caller is introduced
**Reachability:** The engine is constructed by the daemon; this method has no
production caller

Mining job construction and block/header validation use the shared difficulty
calculation path, not this method.

Recommendation: remove the unused interface method, or implement it by
delegating to the same chain-tip candidate calculation. Do not maintain a
second difficulty algorithm.

### D-03 — Anchor/MTP helper functions always return false

**Priority:** P2 removal
**Reachability:** Dormant, definition-only header helpers

`GetAnchorParamsOnChain()` and `GetPrevMtps()` have no callers. Production DAA
does not depend on them.

Recommendation: delete them. If reintroduced, they must use the active-chain
view and the same tested MTP implementation as block validation.

### D-04 — `Phase2ActivateBestChain()` does not validate, connect, or disconnect

**Priority:** P1 removal/quarantine; never wire in as written
**Reachability:** Dormant

The function loads nominal blocks, does not convert them, does not invoke
`BlockValidator`, and marks index entries connected. No production caller was
found; a service comment merely says it should be run.

Recommendation: delete/archive this abandoned orchestration path or place it
behind an unmistakable non-production boundary. If this architecture is
revived, implement it as a new reviewed path with reorg, crash-atomicity, undo,
and neuter tests rather than completing the stubs piecemeal.

### D-05 — Phase-2 block-index/undo adapters return fabricated results

**Priority:** P1 removal/quarantine
**Reachability:** Dormant; no construction or callers found

The adapters return `nullptr` for block indexes, false for block loading, true
for every undo-existence query, zero for written checksums, and ignore expected
checksums on read. They support the dormant Phase-2 path above.

Recommendation: remove them with that path. If retained for a future migration,
make every unsupported operation fail closed—especially `hasUndo()`—and keep
them out of production targets until contract tests cover real storage.

## Non-defect or documentation-only markers

### N-01 — Shielded “aggregated range proof TODO”

**Reachability:** Live-consensus, but the named missing verifier is not missing

The field `aggregated_range_proof` is a container for one legacy Borromean
range proof per value commitment. `VerifyBundleRangeProofs()` decodes it,
requires the proof count to match the value commitments, and calls
`secp256k1_rangeproof_verify()` for every entry. Active shielded validation
invokes it and rejects empty required proofs.

The TODO means a true BPPP aggregated verifier has not replaced this design.
Recommendation: rename the field/container or make “legacy per-cv proof list”
prominent in the normative shielded specification. Treat any future aggregated
proof as a new protocol with domain separation, encoding, activation, and
independent review—not as filling in a local verifier stub.

### N-02 — Duplicate 128-byte header assertion

The “remove after Phase 3” marker guards an already-enforced header-size rule.
It is redundant but fail-closed. Remove the duplicate only as cleanup after a
test pins the primary rule.

### N-03 — UTXO type and lock-API comments

The block validator comment naming `wallet::UTXOIndex` is stale: the shown path
already consumes consensus `UTXOEntry` through the provider interface. The
global-UTXO lock marker is sample documentation, not executable code.

Recommendation: correct/remove the stale comments so future audits do not
mistake them for live dependency violations.

### N-04 — Intentional placeholders and platform telemetry

- Freeze-manifest `TODO_COMPUTE_HASH` is explicitly rejected; it is a
  fail-closed sentinel.
- Utreexo zero hashes/positions are algorithmic placeholders in data
  structures, not incomplete validation.
- Legacy 96-byte shielded-output placeholder notes document compatibility.
- Windows CPU usage returning 0 affects monitoring only.

These should not be included in the consensus remediation queue.

## Recommended issue sequence

1. **CCV safety and chain-state evidence:** publish the revealed-use scan,
   inventory operator-known hidden covenant leaves, specify successor binding,
   adversarially test it, and choose explicit activation semantics.
2. **Tapleaf length rule:** decide whether to standardize the deployed 252-byte
   rule or activate full canonical CompactSize behavior.
3. **Startup block-data validation:** make the existing startup claim true.
4. **Delete/quarantine dormant consensus architecture:** Phase-2 activation,
   its adapters, duplicate ASERT, unused anchor helpers, and the constant
   difficulty interface.
5. **Header-sync peer diversity and truthful status.**
6. **Normative shielded specification:** describe the actual per-cv proof
   container and all custom cryptographic transcript/domain rules before
   proposing aggregation.

## Boundaries and follow-up evidence

This pass proves source reachability at the audited revision and, through the
pinned chain scan below, proves that no CCV leaf was revealed on the selected
mainnet history. It cannot reveal unused Taproot leaves. It also does not
establish cryptographic security of the custom shielded protocol; it only
corrects the narrower claim that the range-proof verifier is absent.

Future remediation artifacts should attach executable call-graph or coverage
evidence for live findings and tests that fail under a targeted neuter of each
repaired invariant.

## Mainnet CCV chain evidence

A reproducible, read-only flat-file scanner is included at
`scripts/audit/scan_ccv_chain.py`. It:

- validates every record's FNV-1a checksum;
- mirrors Dinero transaction decoding to locate witnesses;
- parses script opcodes so a `0xbe` byte inside pushed data is not counted;
- follows an explicit RPC tip through previous-block hashes to genesis rather
  than assuming every flat-file record belongs to the active chain; and
- refuses height mismatches, missing ancestors, checksum failures, or two
  transaction sections under the same block header.

Run against the locally synced mainnet node:

| Measurement | Result |
|---|---:|
| Genesis | `0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f` |
| Chain tip | `000000ac150d3a83251b61fc4819d4cd044e916836551d3f6767e74ea25644a4` |
| Tip height | 75,471 |
| Active blocks walked | 75,472 |
| Transactions | 75,548 |
| Inputs | 77,824 |
| Outputs | 228,500 |
| P2TR outputs | 73,536 |
| Taproot script-path witnesses | 0 |
| Revealed Tapscript leaves | 0 |
| Revealed CCV leaves | **0** |

The exact machine-readable result is
`docs/audits/evidence/CCV_MAINNET_SCAN_75471.json`. Six focused unit tests cover
active-chain selection, height pinning, checksum failure, executable CCV
detection, and false-positive exclusion for direct and `PUSHDATA1` pushes. A
neuter changing the scanner's CCV opcode from `0xbe` to `0xbf` makes the
synthetic active-chain test fail exactly at the expected one-versus-zero CCV
count.

The flat file contained 176,936 records representing 75,721 unique block
headers. Of the repeated records, 258 had identical transaction sections but
different trailing block bodies (the optional Utreexo area); 249 unique blocks
were not on the selected active chain. These facts are reported separately and
do not inflate active-chain transaction counts.

Conclusion: no transaction through the pinned tip revealed a covenant script
path of any kind, so CCV has never been exposed to consensus execution on that
chain. This supports “no historical CCV use,” but it does **not** prove “no CCV
output exists.” The 73,536 P2TR outputs reveal only tweaked public keys, not
their unspent script trees. The local datadir also contains no SQLite
`contracts` or `covenant_utxos` tables, but that is local metadata evidence
only—not a network-wide cryptographic proof.
