# Mainnet CTV/CCV activation at block 80,000

Status: scheduled consensus activation; release gated on independent review
and fleet readiness.

Decision date: 2026-08-01.

## Consensus boundary

The first mainnet block enforcing Dinero covenant profile v1 is block 80,000.
The exact chain parameters are:

| Rule | Mainnet height |
|---|---:|
| BIP341 script path | 1 |
| CTV (`OP_CHECKTEMPLATEVERIFY`) | 80,000 |
| CCV (`OP_CHECKCONTRACTVERIFY`) | 80,000 |
| CSFS / CSFSVERIFY | dormant (`UINT32_MAX`) |
| TXHASH | dormant (`UINT32_MAX`) |

Block 79,999 uses the historical meanings: CTV is NOP4 and CCV is a BIP342
`OP_SUCCESS` slot. Block 80,000 removes CTV and CCV from those historical
meanings and enforces the normative profile. Block validation uses the block's
candidate height; mempool admission and block-template selection use the next
candidate height.

The v2 mainnet consensus checksum for these parameters is:

```text
480c4b727fe55dff2c3200adeb45da8014fbacac02572a4dc6ddfa5ed7399d2c
```

CSFS, TXHASH, confidential covenants, shielded covenants, and testnet
activation are not part of this decision.

This is a coordinated flag-day soft fork: there is no miner signalling or
versionbits state machine, and CTV/CCV remain dormant on public testnet. Mainnet
would therefore be the first public network to enforce this profile. Control of
the production fleet makes coordinated deployment possible, but does not
substitute for the independent review, release-candidate evidence, or go/no-go
rules below.

## Decision window

The decision was recorded from a fully synchronized mainnet node at height
76,305, hash
`00000096e12d95e7607d1f701b8477fdb23cc4adfd9321da36eacf159d99220d`.
Height 80,000 therefore provided 3,695 blocks of lead time. Observed production
cadence over the preceding 200, 500, and 1,000 blocks placed that window at
roughly 12–18 days; block height, not wall-clock time, remains authoritative.

## Release gates

The activation binary must not be released to the production fleet until all
of the following are complete:

1. An independent reviewer signs off on the normative CTV/CCV specifications,
   implementation diff, vectors, resource bounds, and activation semantics.
2. The exact release candidate passes `CovenantActivation`,
   `CovenantSystemLifecycle`, `CovenantProfileWallet`,
   `CovenantWalletRecovery`, and `CovenantWalletMultinodeLifecycle`, plus the
   repository's required CI lanes.
3. Release artifacts for every shipped platform report the checksum above and
   contain the reviewed commit.
4. Every validating/mining node is upgraded by height 79,000 and agrees on
   tip, headers, checksum, peer connectivity, and block-template readiness.
5. Operators confirm that CSFS and TXHASH are still dormant and that the
   mainnet covenant wallet/RPC construction guard remains fail-closed unless a
   separate production-wallet change has been reviewed and deployed.

Height 79,000 is the go/no-go checkpoint. If any gate is incomplete there,
operators must schedule a later activation in a new release. They must not
reduce the validation or deployment window to preserve height 80,000.

## Deployment and monitoring

The controlled production fleet has four nodes. LA is retired and is not part
of this rollout.

| Role | Location | Platform/artifact |
|---|---|---|
| Non-mining validator | NA | reviewed Linux `dinerod` artifact |
| Non-mining validator | SJ | reviewed Linux `dinerod` artifact |
| Non-mining validator | EU1 | reviewed Linux `dinerod` artifact |
| Mining node | operator host | separately built macOS mining artifact |

The three validators may share one byte-identical Linux artifact. The mining
node cannot: it requires its own macOS build from the same reviewed commit.
Record and verify each platform's binary hash independently; equality of the
Linux and macOS binary hashes is neither expected nor required. Every node must
nevertheless report the same consensus checksum.

1. Record each node's pre-upgrade height, best hash, binary hash, version, and
   consensus checksum.
2. Build the Linux validator artifact and the separate macOS mining artifact
   from the exact reviewed commit. Record each artifact's SHA-256 digest and
   verify the embedded/reported consensus checksum before deployment.
3. Upgrade NA as the non-mining canary and verify full convergence. Then upgrade
   SJ and EU1, one at a time, verifying convergence after each. Upgrade the
   macOS mining node last and confirm block-template readiness. Do not allow
   mixed consensus binaries to reach block 80,000.
4. After every upgrade, verify the checksum above through RPC, confirm the
   node's binary hash matches its recorded platform artifact, and compare the
   same best block across all four nodes. The mining node's macOS hash and RPC
   checksum must be recorded explicitly rather than inferred from the Linux
   validators.
5. At height 79,000, enumerate NA, SJ, EU1, and the mining node by name and
   obtain a four-of-four readiness record. A missing node is a failed gate, not
   an implicit exclusion.
6. At heights 79,999 and 80,000, record the accepted block hash, validation
   status, peer count, mempool state, and block-template result on every node.
7. Keep automatic covenant creation disabled initially. After the activation
   block is stable, use a deliberately small reviewed CTV/CCV canary before
   placing material value under the new rules.

The wallet/RPC profile remains regtest-only in this activation change.
Consensus is available to independently reviewed transaction builders at the
boundary, but the project wallet must not imply production construction support
until its separate chain guard, packaging, recovery, and client rollout are
approved.

## Abort and recovery rules

- Before block 80,000, a discovered defect requires a coordinated superseding
  release with a later activation height. Every validator must move together.
- At or after block 80,000, changing the opcode meanings again is another
  consensus change. Do not roll back one node or publish an ad-hoc binary.
- If monitoring detects disagreement, stop mining and covenant creation,
  preserve all logs/datadirs, compare exact binaries and checksums, and resolve
  the fleet on one reviewed rule set before resuming.

No historical-use scan can reveal hidden Taproot leaves. The activation safety
case rests on the frozen specification, implementation evidence, independent
review, and coordinated deployment—not on assuming hidden covenant leaves do
not exist.
