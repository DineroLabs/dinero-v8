# Shielded v1 block-commitment audit — 2026-07-29

## Result

No exercised same-block-hash/different-shielded-state condition was found on the
active mainnet chain through height 75,479.

Version-5 transactions exclude the shielded bundle from txid, so their bundle
bytes depend on the coinbase `DINW` witness commitment for block identity.
Shielded activation (8,650) preceded the hard-coded mandatory-`DINW` boundary
(10,670), creating an optional-commitment window of 2,020 blocks.

A read-only RPC scan found five blocks with shielded activity in that window.
Every one has a `DINW` output in its coinbase:

| Height | Block hash | Shielded outputs | Shielded spends | DINW |
|---:|---|---:|---:|:---:|
| 9,998 | `0000004acfdb6ae753931096a1e7d2ee095e83a925a88de0b5df6b7ed40fbf6f` | 1 | 0 | yes |
| 10,087 | `0000003a9015c1d7a794fd12fc8252a609c221014b11154f8109401325b9cb20` | 1 | 0 | yes |
| 10,088 | `0000002ab49510ef3342552e5f3950f95ebaba8f8c8abaad534489e8b7087015` | 2 | 1 | yes |
| 10,133 | `000000143f1a4c06bedc0701eb35ea93b17cfeee1863bb81934e58c26891d86b` | 1 | 0 | yes |
| 10,158 | `0000007bb72347d312e25a33db5291bdc989bfac616eae452deae51c53cc1393` | 2 | 2 | yes |

The window contains 2,049 transactions in total. Fourteen blocks contain a
non-coinbase transaction. The shielded output feed identifies the five
shielded blocks above; the remaining final-window blocks contain no
non-coinbase transaction after height 10,316.

## Why the question existed

The following implementation facts compose:

1. `Transaction::Serialize(false)` excludes a v5 shielded bundle, so
   `GetTxid()` does not bind it.
2. `ComputeMerkleRoot()` uses txid.
3. BlockHeader v1 has no direct shielded-state root; its reserved bytes remain
   zero.
4. `ComputeWitnessMerkleRoot()` uses wtxid, and wtxid includes the v5 bundle.
5. A coinbase `DINW` commitment therefore closes the block-identity gap when it
   is present and validated.
6. `ValidateWitnessCommitment()` accepts an absent commitment.
7. `BlockValidator` separately requires it for witness-bearing blocks only at
   the hard-coded height 10,670 and above.
8. The honest block assembler adds `DINW` whenever full rules are active, which
   is height 0 on mainnet.

Thus the optional window was a consensus-permitted condition but not an
honest-miner condition. The active-chain scan proves that every actually
shielded block in that window followed the honest path.

## Remaining defect: policy-source drift

`ChainParams` says mainnet witness commitment enforcement is enabled from
height 1. Production block validation does not consume those fields.
`EnforceWitnessCommitment()` consumes them only in tests, while
`BlockValidator` uses a separate hard-coded height 10,670.

Tracking issue: [#431](https://github.com/DineroLabs/dinero-v8/issues/431).

This is not a current shielded inflation finding:

- current mainnet height is well past 10,670;
- current witness-bearing blocks require and validate `DINW`;
- the pre-value-commitment pool was discarded at shielded epoch reset 61,000.

It is still consensus-maintenance debt: two advertised activation sources
disagree, and changing either casually could invalidate history or split nodes.

## Recommendations

1. Keep this five-block evidence as a regression fixture or reproducible
   active-chain scan.
2. Open a dedicated issue for the unused chain-parameter fields and hard-coded
   10,670 boundary.
3. Before changing the boundary, pin mainnet/testnet/regtest historical behavior
   with activation tests and decide which source becomes authoritative.
4. Prefer version 6 for newly constructed shielded transactions. Any future
   rejection of new version-5 bundles requires an explicit activation rule;
   historical v5 validation must remain intact.
5. Do not activate the dormant `DSP` shielded-root encoding as an incidental
   fix. A direct state commitment would be a separate consensus protocol.

## Reproduction notes

The audit used the local mainnet RPC in read-only mode:

- `getblockhash` and `getblock` for every height 8,650–10,669;
- `blockchain.shielded.outputs 8650 10669` to identify shielded activity;
- `blockchain.gettransaction <coinbase-txid>` to inspect each candidate
  coinbase script;
- `6a25 444e5257 01 ...` as the `DINW` script prefix.

No chain, wallet, mempool, or node setting was modified.
