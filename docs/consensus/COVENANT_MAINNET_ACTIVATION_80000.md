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

For the controlled three-node fleet:

1. Record each node's pre-upgrade height, best hash, binary hash, version, and
   consensus checksum.
2. Upgrade one non-mining node, verify it fully converges, then upgrade the
   remaining non-mining node and the mining node. Do not allow mixed consensus
   binaries to reach block 80,000.
3. After every upgrade, verify the checksum above through RPC and compare the
   same best block across all nodes.
4. At heights 79,999 and 80,000, record the accepted block hash, validation
   status, peer count, mempool state, and block-template result on every node.
5. Keep automatic covenant creation disabled initially. After the activation
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
