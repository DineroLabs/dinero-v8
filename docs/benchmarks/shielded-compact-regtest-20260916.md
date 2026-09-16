# Compact proof mining on isolated regtest

Status: experimental implementation; Linux qualification pending. No production
activation or deployment. This advances the design in
`docs/specs/shielded-compact-activation-test-plan.md`; that document describes
the broader review matrix, not completed coverage.

## Isolation and format

- `DINERO_ENABLE_COMPACT_REGTEST` defaults OFF. Ordinary builds do not recognize
  the experimental version and explicitly reject the new activation flag.
- An enabled build requires regtest and an explicit
  `--consensus-shielded-compact-height=N` strictly after the Auth reset. The
  parameter is dormant (`UINT32_MAX`) by default on every network. The CLI
  rejects mainnet, testnet, sentinel/overflow, negative and malformed heights.
- `0x40000006` is a regtest experiment, not a production protocol assignment.
  Its owner signs the new version. It carries compact ordinary Auth spend and
  output proofs only; private-covenant compact proofs are unsupported.
- The new envelope requires the witness marker and minimal CompactSize lengths.
  Historical v5/v6 parsing retains its existing rules. Explicit fee, original
  compact bundle bytes and the existing Auth signature envelope are preserved.
- Expansion uses circuit-derived profiles 6/4 and a temporary verification
  bundle. Transaction IDs, signatures, Utreexo leaves, blocks and persisted
  bytes use the original transaction. Proof limits remain four spends, two
  outputs and eight proofs per block; smaller bytes buy no extra proof slots.

## Measured local results

macOS arm64 Release, ephemeral regtest; Auth H=2, DNRS H=3, compact H=124.
Two full nodes and one stateless node mine through the daemon template path.

| Shape | Actual serialized transaction | Fee observation |
| --- | ---: | --- |
| Shield | 27,906 bytes | Fixed test fee; no automatic-fee claim |
| Transfer, one spend/two outputs | 97,094 bytes | Fixed test fee; no automatic-fee claim |
| Unshield, taproot recipient | 42,053 bytes / 42,052 vbytes | 42,068 una at 1 una/vbyte plus the existing 16-una margin |

The corresponding ordinary unshield costs 75,761 una for 75,745 vbytes.
That is about 44.5% lower paid unshield fees in this regtest experiment.
Observed compact wallet RPC times include 9.402, 9.736, 13.701 and 9.651 seconds
under differing concurrent build/test loads. These are not a controlled speed
comparison and do not establish further proving-speed improvement.

## Completed checks

- Test-first compact wallet construction, height/network admission, canonical
  envelope parsing and full-proof/compact-version rejection. Removing the
  height guard makes the H-1 assertion fail; the restored guard passes.
- Same valid signed proof rejected at H-1, accepted at H/H+1, rejected again at
  H-1 and on the wrong network. Version substitution invalidates its signature.
- Original raw transaction and mined block bytes agree. An independent Python
  parser recomputes txid/wtxid and shows the expanded verification view has a
  different txid.
- Independent HashUTXOV2/proof reconstruction uses the original compact
  outpoint and actual inclusion height. Wrong expanded-view ID, output index,
  amount, script or height fails against the captured root.
- Mine the exact signed transparent child, prove the current unshield leaf is
  gone, and verify the previously captured proof against its historical root.
  Disconnect/reconnect restores/removes the correct leaf and commitment.
- Full-node/CSN combined state hashes agree after each named block, parent and
  child restarts, manual disconnect/reconnect, full reindex and final restart.
- Reorg below H, inspect the H-1 template, mine its replacement block and
  exclude compact transactions and descendants; reconsider the original chain
  and recover the original combined state hash.
- A normal build explicitly rejects genuine compact wire bytes without changing
  its tip or mempool. The experiment is incompatible with unchanged peers.
- Dedicated Linux workflow builds both compile-time configurations, executes
  `CompactRegtestLifecycle`, runs the proof suites and checks the unique CTest
  inventory for missing execution. Its baseline has no exemptions.

## Recovery defects found during qualification

These reproduce without compact transactions and have separate draft reviews:

- #758: manual CSN invalidation moved the tip without rewinding its forest.
  Reconstruct/verify the parent forest before disconnect, and use existing
  verified replay during reconsideration. Ordinary coinbase-only rewinds to
  heights 4, 2 and 0 pass, as does existing network reorg coverage.
- #759: reindex rebuilt SQLite nullifiers but omitted authoritative ChainDB
  rows. Startup correctly discarded cache-only records and refused the marker
  mismatch. The fix stages validated nullifiers and epoch resets with the block
  batch; ordinary shield/transfer/unshield reindex and restart now pass.

## Remaining production gates

Linux results, independent format/consensus review, fixed-byte cross-architecture
vectors, the remaining hostile-input/resource-boundary matrix, instrumented
daemon/wallet qualification, and separate pool protocol qualification remain.
The pool repository and production nodes were not changed by this experiment.
Any production version assignment, activation height or coordinated rollout
requires its own review and owner authorization.
