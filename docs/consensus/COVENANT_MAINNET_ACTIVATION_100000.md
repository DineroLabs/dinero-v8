# Mainnet CTV/CCV activation at block 100,000

Status: **RE-AUTHORIZED** on 2026-08-30 after the restarted review and
exact-release assurance work. Block 100,000 activates CTV and CCV together.

The complete upstream BIP119 transaction corpus exposed a material
P2WSH/Taproot CLEANSTACK semantic defect after the original freeze. The defect
was fixed and the review, mutation, resource, restart/reorg, and multi-host
gates were rerun. The authorized consensus checksum is:

```text
68e0a99766e8ab1224ee040ec715bbbd0a544a59d4b3a96025dd35f77f4e960a
```

See `../audits/COVENANT_DEFERRAL_2026-08-22.md` for the historical stop and
the readiness evidence package for the subsequent re-authorization.

Decision date: 2026-08-01.

## Consensus boundary

The first mainnet block enforcing Dinero covenant profile v1 is block 100,000.
The exact chain parameters are:

| Rule | Mainnet height |
|---|---:|
| BIP341 script path | 1 |
| CTV (`OP_CHECKTEMPLATEVERIFY`) | 100,000 |
| CCV (`OP_CHECKCONTRACTVERIFY`) | 100,000 |
| CSFS / CSFSVERIFY | dormant (`UINT32_MAX`) |
| TXHASH | dormant (`UINT32_MAX`) |

Block 99,999 uses the historical meanings: CTV is NOP4 and CCV is a BIP342
`OP_SUCCESS` slot. Block 100,000 removes CTV and CCV from those historical
meanings and enforces the normative profile. Block validation uses the block's
candidate height; mempool admission and block-template selection use the next
candidate height.

The v2 mainnet consensus checksum for these parameters is:

```text
68e0a99766e8ab1224ee040ec715bbbd0a544a59d4b3a96025dd35f77f4e960a
```

CSFS, TXHASH, confidential covenants, shielded covenants, and testnet
activation are not part of this decision.

This is a coordinated flag-day soft fork: there is no miner signalling or
versionbits state machine, and CTV/CCV remain dormant on public testnet. Mainnet
would therefore be the first public network to enforce this profile. Control of
the production fleet makes coordinated deployment possible, but does not
substitute for reproducible assurance evidence, release-candidate results, or
the go/no-go rules below. External review remains invited and valuable; the
release gate does not depend on purchasing a commercial audit or waiting
indefinitely for a volunteer reviewer.

## Everything is measured in block height

Every gate, window, and deadline in this document is expressed in **block
height**. No gate is defined in calendar time.

This is not a stylistic preference. Observed block spacing has ranged from
~36 s to ~147 s against a 120 s target — a 4x spread — so any fixed number of
days corresponds to a different, unknowable number of blocks. A "14-day review"
could mean 10,000 blocks or 20,000 depending on when it happened to run. Height
is what consensus actually enforces, it is identical on every node, and it is
not subject to clock skew or interpretation.

Wall-clock figures appear in this document only as **informational estimates**,
always labelled as such. They never define a gate.

## Two independent gates

Shipping the binary and authorizing the opcodes are **separate decisions with
separate evidence**. Conflating them costs activation runway for no safety
benefit: the covenant opcodes are dormant until block 100,000 regardless of when
the binary is deployed, so deploying early buys real soak time on the production
fleet while public review runs in parallel.

| | Gate A — merge, release, deploy | Gate B — activation authorization |
|---|---|---|
| Authorizes | merging #480, cutting the release, upgrading the fleet | the opcodes becoming enforceable at 100,000 |
| Evidence | CI, deterministic reproducible evidence, reference model, mutation/fuzz/resource/lifecycle/reorg tests, artifact digests | review window closed, critical/high findings resolved, residual risk accepted in writing |
| Deadline | as early as possible; all nodes upgraded **by height 99,000** | **by height 99,000** |
| If unmet | do not merge or deploy | ship a coordinated activation deferral |

**Gate A does not require Gate B.** Code may be merged, released, and deployed
while the review window is still open. **Gate B never relaxes Gate A**: a
completed review does not authorize deploying a binary whose CI evidence is
incomplete.

## Gate A — merge, release, and deployment readiness

1. The final synchronized head passes all required CI lanes, verified from the
   logs by exact test name rather than from a badge, and the CI run's head SHA
   matches the merge candidate.
2. Covenant evidence is reproducible from a clean checkout: BIP-119 vector
   corpus, CCV adversarial suite, implementation-independent CCV reference
   model, mutation coverage, sanitizer-fuzz, bounded-state enumeration,
   resource limits, lifecycle, and reorg evidence.
3. Every covenant evidence test is always-on. Tests gated solely on `assert()`
   do not count as evidence: CI builds Release, `NDEBUG` is defined, and
   `assert(x)` compiles to `((void)0)`. See issue #497.
4. Release artifacts are built from the exact assurance-frozen commit for every
   shipped platform, with SHA-256 digests recorded per platform. Linux and macOS
   digests will differ and are recorded independently; the consensus checksum
   must match across both.
5. `CovenantActivation`, `CovenantSystemLifecycle`, `CovenantProfileWallet`,
   `CovenantWalletRecovery`, and `CovenantWalletMultinodeLifecycle` pass on the
   exact release candidate.

## Gate B — activation authorization

1. The normative specification and release commit are frozen. Record the
   **freeze height** — the chain height at the moment of freezing — and the
   exact commit the published review package identifies.
2. The public review window is `[freeze height, 99,000]`. It opens the moment
   the package is published and runs to the go/no-go checkpoint. There is no
   separate duration to satisfy and no calendar deadline: publishing earlier is
   the only way to lengthen review, which is the incentive we want.
3. All reported critical and high findings are resolved.
4. The owner records explicit acceptance of the residual risk that no paid
   external audit was obtained.
5. Every validating and mining node is upgraded and agrees on tip, headers,
   consensus checksum, peer connectivity, and block-template readiness, with a
   four-of-four named readiness record at height 99,000.
6. CSFS and TXHASH are confirmed still dormant, and the mainnet covenant
   wallet/RPC construction guard remains fail-closed.

Height 99,000 is the go/no-go checkpoint. If any Gate B item is incomplete
there, operators must ship a coordinated activation deferral in a new release.
They must **not** shorten the review or deployment window to preserve height
100,000.

### Review-window restart rule

The review window restarts — the freeze height is reset to the current height —
on any material change to **covenant consensus semantics** after the window
opens: opcode behavior, activation heights, resource limits, or the serialized
forms the opcodes commit to.

The window does **not** restart for test-only changes, provenance or evidence
records, or editorial corrections.

Record every post-freeze change with its classification, the height at which it
landed, and the resulting restart decision, so the judgement is auditable rather
than remembered.

A restart late in the window can make Gate B unreachable before 99,000. That is
the intended behavior: it forces a deliberate deferral rather than a shortened
review.

## Height tracking (informational)

Wall-clock projections are informational and never define a gate. They exist
only so a deferral can be decided while slack remains rather than at the
checkpoint.

Reference measurement, recorded so future readers can judge drift rather than
re-derive it: over the 960 blocks ending at height 77,940, median spacing was
**59.4 s/block** against the 120 s target, with 7 of 8 sampled 120-block windows
below target. Difficulty was rising across those samples, so ASERT is expected
to pull spacing back toward target; that had not yet been observed to persist.
At the target, 77,940 → 99,000 is ~29 days; at the measured median, ~14.5 days.

Because those differ by ~2x, track rather than assume: recompute the projected
arrival of height 99,000 from a rolling 720-block window, record each projection
with the height and window it came from, and escalate if the projection moves
earlier than expected. A single fast window is not a sustained pace.

## Deployment and monitoring

The controlled production fleet has four nodes. LA is retired and is not part
of this rollout.

| Role | Location | Platform/artifact |
|---|---|---|
| Non-mining validator | NA | assurance-frozen Linux `dinerod` artifact |
| Non-mining validator | SJ | assurance-frozen Linux `dinerod` artifact |
| Non-mining validator | EU1 | assurance-frozen Linux `dinerod` artifact |
| Mining node | operator host | separately built macOS mining artifact |

The three validators may share one byte-identical Linux artifact. The mining
node cannot: it requires its own macOS build from the same assurance-frozen
commit. Record and verify each platform's binary hash independently; equality
of the Linux and macOS binary hashes is neither expected nor required. Every
node must nevertheless report the same consensus checksum.

1. Record each node's pre-upgrade height, best hash, binary hash, version, and
   consensus checksum.
2. Build the Linux validator artifact and the separate macOS mining artifact
   from the exact assurance-frozen commit. Record each artifact's SHA-256
   digest and verify the embedded/reported consensus checksum before
   deployment.
3. Upgrade NA as the non-mining canary and verify full convergence. Then upgrade
   SJ and EU1, one at a time, verifying convergence after each. Upgrade the
   macOS mining node last and confirm block-template readiness. Do not allow
   mixed consensus binaries to reach block 100,000.
4. After every upgrade, verify the checksum above through RPC, confirm the
   node's binary hash matches its recorded platform artifact, and compare the
   same best block across all four nodes. The mining node's macOS hash and RPC
   checksum must be recorded explicitly rather than inferred from the Linux
   validators.
5. At height 99,000, enumerate NA, SJ, EU1, and the mining node by name and
   obtain a four-of-four readiness record. A missing node is a failed gate, not
   an implicit exclusion.
6. At heights 99,999 and 100,000, record the accepted block hash, validation
   status, peer count, mempool state, and block-template result on every node.
7. Keep automatic covenant creation disabled initially. After the activation
   block is stable, use a deliberately small reproducible CTV/CCV canary before
   placing material value under the new rules.

The wallet/RPC profile permits public descriptor construction on mainnet but
fails closed on spend construction until the next candidate block is at least
100,000. Testnet remains unsupported because the profile is dormant there.

## Abort and recovery rules

- Before block 100,000, a discovered defect requires a coordinated superseding
  release with a later activation height. Every validator must move together.
- At or after block 100,000, changing the opcode meanings again is another
  consensus change. Do not roll back one node or publish an ad-hoc binary.
- If monitoring detects disagreement, stop mining and covenant creation,
  preserve all logs/datadirs, compare exact binaries and checksums, and resolve
  the fleet on one reviewed rule set before resuming.

No historical-use scan can reveal hidden Taproot leaves. The activation safety
case rests on the frozen specification, reproducible implementation evidence,
public review opportunity, explicit residual-risk acceptance, and coordinated
deployment—not on assuming hidden covenant leaves do not exist.
