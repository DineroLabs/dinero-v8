# Trustless Light-Client Shielded Scanning - M2 Sanity Log

Date: 2026-05-27
Branch: `feature/m2-shielded-output-feed`
Head under test: `9ec0e398`
Machine: macOS arm64 dev workstation

## Build And Test

Commands:

```bash
cmake -S . -B build-m2
cmake --build build-m2 --target dinerod test_shielded_output_feed -j8
ctest --test-dir build-m2 -R '^ShieldedOutputFeed$' --output-on-failure
ctest --test-dir build-m2 -R '^ShieldedOutputsRpcFeed$' --output-on-failure
```

Results:

| Check | Result |
| --- | --- |
| Configure | PASS |
| `dinerod` build | PASS |
| `test_shielded_output_feed` build | PASS |
| `ShieldedOutputFeed` | PASS, 13 tests, 0.01 s |
| `ShieldedOutputsRpcFeed` | PASS, 21.47 s |

The integration test starts a real regtest daemon, mines spendable funds, creates a `wallet.shield` output, creates a shielded self-transfer, and verifies `blockchain.shielded.outputs` through the actual persisted block/RPC path.

## Source-Truth Correction

The first regtest pass caught a real mismatch between the plan and current daemon behavior: `wallet.shield` and legacy self-transfer change outputs still emit 96-byte encrypted-note placeholders, while newer addressed outputs use the 611-byte `epk || ct || tag` note shape.

M2 now copies `encrypted_note` bytes verbatim instead of rejecting non-611-byte notes. This is required for commitment-tree completeness: a light client must append every public commitment leaf even when an old placeholder note is not trial-decryptable. Clients should trial-decrypt only notes with a supported length and still use all returned commitments for local anchors.

Malformed bundle bytes still fail loudly; note length alone is not a consensus-validity failure.

## RPC Fixture

Regtest fixture:

| Field | Value |
| --- | --- |
| Tip height | 103 |
| Shield tx height | 102 |
| Transfer tx height | 103 |
| Returned feed blocks | 2 |
| Returned outputs | 2 |
| Returned nullifiers | 1 |

Validated JSON behavior:

- empty non-shielded blocks are omitted from `result.blocks`
- negative `from_height` clamps to 0
- oversized `count` clamps to 2000
- `wallet.shield` block returns exact txid, commitment, leaf index 0, and the 96-byte legacy encrypted note
- `wallet.transfer` block returns the public spend nullifier plus the next output at leaf index 1
- `shieldedoutputs` alias works with positional params
- multi-block range returns height order with monotonic leaf indexes

## Payload And Latency

Measured with local JSON-RPC over loopback using the regtest fixture above.

| Request | Latency | Response bytes |
| --- | ---: | ---: |
| `blockchain.shielded.outputs {from_height:0,count:2000}` | 13.209 ms | 1,831 |
| `blockchain.shielded.outputs {from_height:102,count:2}` | 12.825 ms | 1,830 |
| `blockchain.shielded.outputs {from_height:103,count:1}` | 13.007 ms | 1,111 |
| `getblock <shield_block> 0` | 8.579 ms | 19,852 |
| `getblock <transfer_block> 0` | 9.923 ms | 45,578 |

For the two shielded blocks in the fixture, the feed response is 1,830 bytes versus 65,430 bytes for fetching the two full raw blocks: about 35.8x smaller. The exact production ratio will vary with transparent transaction volume, shielded proof sizes, and whether outputs are legacy 96-byte placeholders or 611-byte decryptable notes.

## Leaf-Index Walk

The local fixture only has 103 blocks, so this log does not claim a full 22k-block mainnet replay benchmark. A temporary attempt to pad an isolated regtest chain to a 2000-block tip was stopped after several minutes because block generation, not the feed RPC, dominated the run.

What is measured here:

- current helper correctness over real persisted blocks
- RPC shape and alias behavior through the daemon
- small-chain leaf-index derivation latency over loopback
- payload savings versus full block fetch for real daemon-generated shielded bundles

What remains if latency becomes a concern:

- run the same RPC against a copied mainnet datadir, not the live operator datadir
- measure `from_height` near tip to isolate the O(chain height) leaf-index walk
- add the planned ChainDB sidecar cache keyed by block hash only if that measurement shows pressure

Current conclusion: on-demand extraction is acceptable for M2. The code stays simple; caching remains a measured follow-up, not speculative complexity.
