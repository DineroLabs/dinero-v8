# Mainnet CTV/CCV activation at block 100,000

Status: scheduled consensus activation; release gated on reproducible
open-source assurance and fleet readiness.

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

## Decision window

The final height decision was recorded from a fully synchronized mainnet node
at height 76,663, hash
`000000837e04943234476ea9fb5465d4939407ca80120d39606d7f1652297b7a`.
Height 100,000 therefore provided 23,337 blocks of lead time. At the consensus
target spacing of 120 seconds that is roughly 32 days, but recent production
cadence has varied materially. Block height, not wall-clock time, remains
authoritative.

## Release gates

The activation binary must not be released to the production fleet until all
of the following are complete:

1. The open-source assurance record is complete: the normative specification
   and release commit are frozen; an implementation-independent CCV reference
   model agrees with production across randomized valid and invalid
   transitions; property, sanitizer-fuzz, mutation, bounded-state, resource,
   lifecycle, and reorg evidence is reproducible from a clean checkout; the
   public review package has been announced for at least 14 calendar days and
   closes before height 99,000; all reported critical/high findings are
   resolved; and the owner records explicit acceptance of the residual risk
   that no paid audit is required.
2. The exact release candidate passes `CovenantActivation`,
   `CovenantSystemLifecycle`, `CovenantProfileWallet`,
   `CovenantWalletRecovery`, and `CovenantWalletMultinodeLifecycle`, plus the
   repository's required CI lanes.
3. Release artifacts for every shipped platform report the checksum above and
   contain the assurance-frozen commit.
4. Every validating/mining node is upgraded by height 99,000 and agrees on
   tip, headers, checksum, peer connectivity, and block-template readiness.
5. Operators confirm that CSFS and TXHASH are still dormant and that the
   mainnet covenant wallet/RPC construction guard remains fail-closed unless a
   separate production-wallet change has been reviewed and deployed.

Height 99,000 is the go/no-go checkpoint. If any gate is incomplete there,
operators must schedule a later activation in a new release. They must not
reduce the validation or deployment window to preserve height 100,000.

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

The wallet/RPC profile remains regtest-only in this activation change.
Consensus is available to transaction builders at the boundary, but the
project wallet must not imply production construction support until its
separate chain guard, packaging, recovery, and client rollout are approved.

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
