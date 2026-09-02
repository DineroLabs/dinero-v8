# Covenant profile v1 activation readiness — 2026-08-30

## Verdict

**Candidate engineering: PASS. Mainnet release: NO-GO until coordinated binary
rollout is complete and the safety checker reports the candidate checksum on
the required fleet.**

CTV and CCV are coupled at mainnet block 100,000. The reviewed candidate is
`437aa230d3d220e556764a89e9e46607f7e23790`, with consensus checksum
`68e0a99766e8ab1224ee040ec715bbbd0a544a59d4b3a96025dd35f77f4e960a`.
The live Mac observation at height 98,919 reported the old checksum
`48bb4b27879a492dd8a83fd1e4826ec422f6b9ac3b1ae6797c9469783036c76e`.
That mismatch is an intentional release blocker, not permission to deploy a
single node independently.

## Exact-boundary and compatibility evidence

- CTV and CCV flags are absent at 99,999 and present together at 100,000.
- Invalid CTV remains accepted at 99,999 and is rejected at 100,000 and later.
- A simulated legacy node continues accepting the invalid activation-height
  transaction, proving operators must upgrade before activation.
- Mempool admission, block selection, restart persistence, rollback below the
  boundary, and reactivation are covered by the 99,998–100,001 lifecycle test.
- Mainnet wallet descriptor construction is available before activation, while
  spend creation fails closed until the next candidate block is active.

## Local Apple Silicon gate

`./scripts/covenant-readiness.sh --build-dir build-qt-release-ui --mutation`
passed 15/15 covenant tests, the funded two-daemon CTV/CCV lifecycle, restart
and reorg recovery, architecture checks, and 16/16 mutation kills.

Near-limit Apple M4 Max measurements:

- 95,416-byte CTV validation: 748.5 microseconds with precomputation.
- 95,416-byte Taproot sighash: 2,389 microseconds with precomputation.
- 8,192-output CCV transition: 57.3 microseconds.
- benchmark peak RSS: 12,812,288 bytes.

## Exact x86_64 gate

Dell built the exact candidate from the reviewed git bundle and passed the
same canonical gate:

- 15/15 covenant tests.
- funded CTV and owner-authorized CCV construction, relay, confirmation,
  restart, rollback, re-relay, and reconfirmation.
- 16/16 mutations caught (100%).
- resource gate: CTV 1,682 microseconds, Taproot 4,960 microseconds, CCV 167.4
  microseconds.

Artifact checksums copied byte-for-byte to DineroTX:

- `dinerod`: `37ba8136429da73424491c9951f3c471633843ece17254f2738000d7a4bce0a3`
- `dinero-cli`: `c2eaaace1b832eb7b0418ee212bdec55c90d0f6183f79f323dba2a6c5a317b7b`

DineroTX linked the copied daemon without missing libraries and reported
version 8.1.9.

## Physical three-host rehearsal

The Apple Silicon Mac, Dell x86_64, and DineroTX x86_64 ran isolated regtest
nodes using the candidate artifacts.

1. All three synchronized to height 105 and hash
   `43190c9e4ce45657f273c243238aa0b66bcbb41276ac34590bc5f0b4cab501bd`.
2. A real wallet-funded CTV output was created and spent. Funding transaction
   `489bb137e650a37e0fc7ad47eb1e7d3d4626340a1ed8edc6e845d5c7fab55f70`
   and spend transaction
   `7a970892b2557349e9bd5f2afe8fdcd00e1951d39d48fbe8a07dac852d2536f6`
   relayed through both remote peers and confirmed by height 107.
3. DineroTX restarted from the same datadir at height 107 and restored hash
   `01b7acad9914798b879927771e06d78cfcc32d94bb2b3a84d0b7db9da458b7ab`.
4. Mac and Dell were isolated and produced competing height-109 and height-110
   tips. After reconnection, Mac reorged to Dell's longer chain.
5. Restarted DineroTX converged at height 110, headers 110, IBD false, hash
   `49f43b0900aedc132a0ce2e79f2705bc4e42fb529e713d0f7a9d699d2994b810`.

All rehearsal daemons were stopped. Production datadirs and wallet funds were
not touched.

## Client parity

- Dinero-Qt and its bundled daemon rebuilt successfully from candidate commit
  `437aa230d` and passed ad-hoc code-sign verification.
- iOS NodeCore was rebuilt in one invocation for device, simulator, and
  universal macOS slices, retained the same 24-symbol FFI ABI, passed the
  six-slice OpenSSL 3.5.7 audit, and linked in a generic-device app build.
- Android NodeCore mirrors height 100,000 and scheduled-network RPC behavior;
  ARM64 and x86_64 native slices and the debug APK built successfully.
- Mobile and Qt therefore enforce identical consensus and construction gates.
  Dedicated covenant transaction UI remains a product-layer follow-up; it is
  not required for nodes to enforce consensus.

## CI and release controls

`.github/workflows/covenant-readiness.yml` wires the already-proven local
command into a Linux release gate, including the funded lifecycle, resource
report, and mutation report. `tools/covenant_activation_safety.py` is the
read-only preflight: release remains blocked until required production nodes
report the candidate checksum, converge on one tip, retain peers, and remain
below height 100,000.

## Required coordinated rollout

1. Publish signed, reproducible 8.1.9 candidate artifacts and operator hashes.
2. Upgrade the required seed, archival, mining, relay, wallet, and explorer
   fleet in a staged window; never leave only one upgraded production node.
3. Run the safety checker against every required node after each stage.
4. Confirm the candidate checksum, shared tip, peer health, and rollback
   packages before height 100,000.
5. Announce the mandatory operator upgrade and monitor blocks 99,999–100,001.

Until these steps are complete, the correct decision is **NO-GO**.
