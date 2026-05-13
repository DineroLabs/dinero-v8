# DINERO Mainnet Readiness Audit Program

Document ID: `DINERO-MAINNET-AUDIT`
Version: `0.1-draft`
Status: `ACTIVE`
Last Updated (UTC): `2026-02-11`
Release Candidate Tag: `TBD`
Owners: `Consensus`, `Node`, `Mining`, `Wallet`, `iOS`, `RPC`, `Release`

## 1. Launch Policy (Hard Rules)

Mainnet launch is blocked unless all conditions are true:

- [ ] All gates in this document are `PASS`.
- [ ] Zero open `P0` and zero open `P1` defects.
- [ ] Zero unresolved consensus divergences across repeated deterministic runs.
- [ ] Reproducible build verified by at least one independent builder.
- [ ] Signed launch approval from all gate owners and release owner.

Severity policy:

- `P0`: Consensus split, fund loss, chain halt, silent divergence.
- `P1`: Deterministic correctness risk that can become consensus or fund-loss risk.
- `P2`: Reliability/performance/security weakness without immediate catastrophic impact.
- `P3`: Non-blocking quality/documentation issues.

## 2. Evidence Contract (Required for Every Gate)

Each gate must ship evidence, not only checkboxes.

- [ ] Test command or CI job ID
- [ ] Commit SHA
- [ ] Date/time (UTC)
- [ ] Environment (`local`, `CI`, `regtest`, `staging`, `testnet-optional`)
- [ ] Result (`PASS`/`FAIL`) with failure summary if failed
- [ ] Artifact links (logs, traces, dumps, screenshots where applicable)
- [ ] Owner sign-off (name + date)

## 3. Catastrophic Failure Register

Any single item below is auto-`FAIL` for launch:

- Silent Utreexo divergence
- Subsidy miscalculation at halving boundaries
- Unbounded orphan/mempool memory growth under adversarial conditions
- Mining template mismatch with canonical tip
- Wallet misreporting confirmed balance
- Race between block acceptance and forest update
- RPC returning inconsistent tip height/state across methods

## 4. Gate Dashboard

| Gate | Name | Owner | Status | Open P0/P1 | Evidence Bundle | Sign-off |
|---|---|---|---|---|---|---|
| G0 | Monetary + Consensus Freeze |  | `NOT STARTED` |  |  |  |
| G0.5 | Startup Invariant Guardrails |  | `NOT STARTED` |  |  |  |
| G1 | Utreexo Safety |  | `NOT STARTED` |  |  |  |
| G2 | Mining Stack Integrity |  | `NOT STARTED` |  |  |  |
| G3 | P2P Robustness |  | `NOT STARTED` |  |  |  |
| G4 | Wallet Correctness (`dinero-qt`) |  | `NOT STARTED` |  |  |  |
| G5 | iOS Correctness (`DineroDPI`) |  | `NOT STARTED` |  |  |  |
| G6 | RPC + Attack Surface |  | `NOT STARTED` |  |  |  |
| G7 | Economic Attack Simulation |  | `NOT STARTED` |  |  |  |
| G8 | Operational Hardening |  | `NOT STARTED` |  |  |  |
| G9 | Final Rehearsal and Go/No-Go |  | `NOT STARTED` |  |  |  |

Status values: `NOT STARTED`, `IN PROGRESS`, `PASS`, `FAIL`, `WAIVED`.

`WAIVED` requires written risk acceptance by release owner and project lead.

## 5. Gate Specifications

### G0 - Monetary + Consensus Freeze (Non-Negotiable)

Objective: Prove monetary policy and consensus constants are immutable and correct.

Hard fail conditions:

- Genesis block hash cannot be reproduced from source.
- Premine diverges from `ECONOMICS-SPEC.md`.
- Subsidy/halving/cap math mismatch or overflow.
- Coinbase maturity mismatch across consensus/wallet/RPC/tests.

Required checks:

- [ ] Genesis hash reproducibility test
- [ ] Premine amount and destination validation
- [ ] Static asserts for subsidy invariants
- [ ] Unit tests at every halving boundary
- [ ] Long-range simulation for 40 halvings
- [ ] Cross-layer coinbase maturity consistency test

Evidence to attach:

- [ ] Halving boundary test report
- [ ] Long-range monetary simulation logs
- [ ] Static assert coverage references
- [ ] Signed consensus freeze note

### G0.5 - Startup Invariant Guardrails

Objective: Fail fast on corrupted or inconsistent startup state.

Hard fail conditions:

- Node starts despite mismatched forest root.
- Node starts with chain tip/DB mismatch.
- `--selftest` misses known invariant violations.

Required checks:

- [ ] `startup_validator` verifies forest root
- [ ] `startup_validator` verifies chain tip matches DB
- [ ] `startup_validator` verifies subsidy invariant
- [ ] `--selftest` mode executes invariants before full startup
- [ ] Deterministic error codes on invariant failure

Evidence to attach:

- [ ] Startup success/failure matrix
- [ ] Corruption-injection test logs

### G1 - Utreexo Safety

Objective: Prove forest state is consensus-critical, deterministic, and reorg-safe.

Hard fail conditions:

- Forest divergence without immediate block invalidation
- Non-deterministic forest rewind on reorg
- Snapshot restore yielding divergent state

Required checks:

- [ ] Forest root treated as consensus-critical
- [ ] Reorg rewind deterministic across repeated runs
- [ ] Snapshot restore parity against baseline node
- [ ] Proof verification failure marks block invalid
- [ ] `getutxoproof` deterministic response
- [ ] Canonical leaf hash computation (endian-safe)
- [ ] 10k randomized block invariant test
- [ ] Corrupt proof injection fuzz test
- [ ] Reorg stress test depth >= 100

Evidence to attach:

- [ ] Forest invariant reports
- [ ] Fuzz corpus and crash triage
- [ ] Reorg replay logs

### G2 - Mining Stack Integrity

Objective: Prove daemon, Stratum, and miners remain consistent under churn.

Hard fail conditions:

- Template does not reflect canonical tip
- Stale template reuse across reorg
- Share accounting inconsistency or duplicate acceptance

Required checks:

- [ ] Template tip correctness on every update
- [ ] Stale template invalidation on reorg
- [ ] Stratum difficulty adjustment correctness
- [ ] Duplicate share dedupe
- [ ] Share submission race safety
- [ ] Coinbase output structure and payout policy verification
- [ ] Multi-output coinbase consensus validation
- [ ] 24h mining simulation
- [ ] Stale block storm simulation
- [ ] Artificial latency simulation
- [ ] 3-node partition simulation

Evidence to attach:

- [ ] Share accounting reconciliation report
- [ ] Block template correctness traces
- [ ] Partition and latency simulation logs

### G3 - P2P Robustness

Objective: Prove sync and relay logic remain safe under adversarial and failure scenarios.

Hard fail conditions:

- Crash/null dereference during sync/reorg
- Unbounded orphan memory growth
- Non-deterministic `active_tip_` initialization

Required checks:

- [ ] Header-first sync correctness
- [ ] Deterministic IBD `active_tip_` init
- [ ] Bounded orphan handling
- [ ] INV/GETDATA rate limiting
- [ ] Safe hardcoded seed fallback
- [ ] Safe DNS seed fallback
- [ ] No unbounded memory growth
- [ ] Join at height 500k scenario
- [ ] Reorg depth 200 scenario
- [ ] Restart during reorg scenario
- [ ] Corrupted local DB scenario

Evidence to attach:

- [ ] Memory growth profile under attack load
- [ ] Crash-free replay logs for scenario set

### G4 - Wallet Correctness (`dinero-qt`)

Objective: Prove wallet state and UI match canonical chain truth.

Hard fail conditions:

- Incorrect confirmed balance display
- Change misuse causing fund loss risk
- Reorg state confusion (phantom or missing funds)

Required checks:

- [ ] Descriptor wallet determinism
- [ ] Taproot derivation parity (`BIP341/BIP86` as wallet design requires)
- [ ] Wallet-scoped RPC correctness
- [ ] Change address safety rules
- [ ] RBF/CPFP behavior correctness
- [ ] Mempool conflict handling
- [ ] Wallet DB crash recovery
- [ ] Pending vs confirmed UI separation
- [ ] Reorg visibility in UI
- [ ] Mining address visibility
- [ ] Reorg wallet simulation
- [ ] Spend-unconfirmed then reorg scenario
- [ ] Cold-start wallet recovery scenario

Evidence to attach:

- [ ] Wallet consistency test logs
- [ ] UI correctness captures for reorg and balance states

### G5 - iOS Correctness (`DineroDPI`)

Objective: Prove mobile safety, deterministic proofs, and key protection.

Hard fail conditions:

- Private key material in logs
- Insecure secret storage
- Balance divergence after offline/resume/reorg

Required checks:

- [ ] Deterministic proof verification
- [ ] Tiered validation behavior and UX copy verified
- [ ] Strict QR parsing
- [ ] Keychain-only secret storage
- [ ] Background sync safety
- [ ] Minimized network-time dependency
- [ ] Airplane mode then resume scenario
- [ ] Partial proof scenario
- [ ] Server unreachable scenario
- [ ] Reorg while app closed scenario

Evidence to attach:

- [ ] Mobile scenario test matrix
- [ ] Security scan/log scrub report

### G6 - RPC and Attack Surface

Objective: Prove RPC plane is authenticated, validated, and resilient.

Hard fail conditions:

- Unsafe debug RPC exposed on mainnet builds
- Unauthorized state-changing operations
- Panic/crash on malformed JSON

Required checks:

- [ ] Debug RPC exposure review
- [ ] No raw transaction bypass path
- [ ] Strict JSON-RPC schema validation
- [ ] Rate limiting enabled
- [ ] Auth mandatory
- [ ] Malformed JSON panic test
- [ ] Garbage payload fuzzing
- [ ] 10k malformed request test
- [ ] Concurrent RPC flood test

Evidence to attach:

- [ ] RPC hardening report
- [ ] Fuzz and flood test logs

### G7 - Economic Attack Simulation

Objective: Quantify resilience against miner/economic manipulation.

Hard fail conditions:

- Invalid reorg/timestamp behavior not rejected
- Attack simulations produce inconsistent or unsafe chain states

Required checks:

- [ ] 51% reorg attempt simulation
- [ ] Timestamp manipulation simulation
- [ ] Low-difficulty attack simulation
- [ ] Empty block mining simulation
- [ ] Fee sniping simulation
- [ ] Long reorg replay simulation

Evidence to attach:

- [ ] Attack simulation report with expected vs observed behavior
- [ ] Mitigation verification notes

### G8 - Operational Hardening

Objective: Prove release process is deterministic, auditable, and safe by default.

Hard fail conditions:

- Non-reproducible binaries
- Ambiguous or undocumented consensus changes
- Mainnet/non-mainnet network parameter overlap (`regtest` and `testnet` if enabled)

Required checks:

- [ ] Reproducible builds verified
- [ ] Deterministic release tag
- [ ] Commit hash embedded in binary/version output
- [ ] Release notes flag consensus-relevant changes
- [ ] Debug logging disabled by default
- [ ] Mainnet ports distinct from all non-mainnet ports (`regtest` and `testnet` if enabled)
- [ ] Mainnet magic bytes distinct
- [ ] Alert/incident signaling decision documented (if used)

Evidence to attach:

- [ ] Independent build verification record
- [ ] Signed release manifest with checksums

### G9 - Final Rehearsal and Go/No-Go

Objective: Validate full-stack stability under launch-like stress.

Hard fail conditions:

- Any divergence, data corruption, or unresolved `P0/P1`
- Any gate not in `PASS`

Required rehearsal sequence:

- [ ] Launch clean multi-node regtest network from fresh state
- [ ] Mine 10k blocks
- [ ] Randomly kill nodes during operation
- [ ] Randomly restart nodes during operation
- [ ] Run deep reorg stress
- [ ] Run wallet recovery stress
- [ ] Run Stratum reconnect storms
- [ ] Verify no divergence
- [ ] Repeat full rehearsal at least once from fresh state
- [ ] Optional: run public testnet soak (>= 7 days) after regtest PASS

Go/No-Go approvals:

- [ ] Consensus owner
- [ ] Node owner
- [ ] Mining owner
- [ ] Wallet owner
- [ ] iOS owner
- [ ] RPC/security owner
- [ ] Release owner

## 6. CI Job Mapping (Template)

Map each required check to CI before declaring the gate as `PASS`.

| Gate | CI Job ID | Trigger | Required Outcome |
|---|---|---|---|
| G0 | `ci-consensus-monetary-invariants` | PR + nightly | PASS |
| G0 | `ci-consensus-halving-boundaries` | PR + nightly | PASS |
| G0 | `ci-consensus-40-halving-sim` | nightly | PASS |
| G0.5 | `ci-startup-validator` | PR + nightly | PASS |
| G1 | `ci-utreexo-randomized-10k` | nightly | PASS |
| G1 | `ci-utreexo-corrupt-proof-fuzz` | nightly | PASS |
| G1 | `ci-utreexo-reorg-depth-100` | nightly | PASS |
| G2 | `ci-mining-24h-sim` | nightly | PASS |
| G2 | `ci-mining-stale-storm` | nightly | PASS |
| G2 | `ci-mining-latency-partition` | nightly | PASS |
| G3 | `ci-p2p-reorg-depth-200` | nightly | PASS |
| G3 | `ci-p2p-corrupt-db-restart` | nightly | PASS |
| G4 | `ci-wallet-reorg-recovery` | PR + nightly | PASS |
| G4 | `ci-wallet-unconfirmed-reorg` | nightly | PASS |
| G5 | `ci-ios-offline-resume-reorg` | nightly | PASS |
| G5 | `ci-ios-proof-determinism` | nightly | PASS |
| G6 | `ci-rpc-fuzz-malformed-10k` | nightly | PASS |
| G6 | `ci-rpc-concurrent-flood` | nightly | PASS |
| G7 | `ci-econ-attack-sim-suite` | nightly | PASS |
| G8 | `ci-release-reproducible-build` | tag + nightly | PASS |
| G9 | `ci-mainnet-full-rehearsal` | manual + scheduled | PASS |

## 7. Issue Tracking Template

Use this for every finding linked to a gate.

| ID | Gate | Severity | Title | Owner | Status | Opened (UTC) | Target Fix (UTC) | Evidence |
|---|---|---|---|---|---|---|---|---|
| `AUD-0001` | `G?` | `P?` |  |  | `OPEN` |  |  |  |

## 8. Sign-off Record

Final launch sign-off requires all entries completed.

| Role | Name | Decision (`APPROVE`/`REJECT`) | Date (UTC) | Notes |
|---|---|---|---|---|
| Consensus Lead |  |  |  |  |
| Node Lead |  |  |  |  |
| Mining Lead |  |  |  |  |
| Wallet Lead |  |  |  |  |
| iOS Lead |  |  |  |  |
| RPC/Security Lead |  |  |  |  |
| Release Lead |  |  |  |  |
| Project Lead |  |  |  |  |

---

If any gate regresses from `PASS` to `FAIL` after sign-off, release status automatically reverts to `BLOCKED`.
