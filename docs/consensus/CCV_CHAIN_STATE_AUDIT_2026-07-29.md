# CCV chain-state audit — 2026-07-29

## Decision

No historical execution of `OP_CHECKCONTRACTVERIFY` (CCV) was found in the
available mainnet archival block store. No supported production wallet or RPC
construction path, covenant database state, or CCV-related node log entry was
found.

This evidence supports the operator's Case A statement that CCV was never
used. It does **not** turn Taproot opacity into a cryptographic proof that no
unspent P2TR output contains a hidden CCV leaf. Consequently:

- the successor-binding change remains dormant on mainnet and testnet;
- this audit is an activation prerequisite, not an activation decision;
- a future activation still requires wallet/RPC work, multi-node lifecycle
  tests, external review, and an explicit deployment height.

## Historical-chain scan

The `ccv-chain-audit` tool reads every `blkNNNNN.dat` record, verifies the
record's FNV-1a checksum, and parses the body with the node's canonical
`Block::Deserialize` implementation. For every input it recognizes a BIP341
script-path witness (including the trailing-annex form), parses the revealed
script's push operations, and counts `0xbe` only when it is an opcode. A
`0xbe` byte inside pushed data is not a match.

The scan was anchored to the node's RPC-reported mainnet tip:

| Field | Result |
|---|---:|
| Branch base | `ffb23da78` |
| Network magic | `0xd1a0c0de` |
| RPC height | 75,474 |
| Expected tip | `000000b03bc0b2b86aea1e61acf7f8f26a65d708842ee15c3ebb89f6155b9bb5` |
| Expected tip present in block store | yes |
| Block files | 1 |
| Bytes scanned | 62,686,434 |
| SHA-256 of the scanned prefix | `9bb8398a0b3262bff66832f62c953c964df89f0c604cafb44c4bf7ae5e906372` |
| Checksum-valid block records | 176,947 |
| Transactions | 177,078 |
| Inputs | 179,908 |
| Outputs | 534,274 |
| P2TR outputs | 171,583 |
| Revealed Tapscript spends | **0** |
| Revealed CCV opcode matches | **0** |
| Malformed revealed scripts | 0 |

The block store contains more records than the active-chain height because it
also retains non-active/stale block bodies. Scanning all stored records is
strictly broader than scanning only the active chain for the question "was a
CCV leaf ever revealed here?"

Reproduction:

```sh
cmake --build build --target ccv-chain-audit
./build/ccv-chain-audit \
  --blocks-dir /absolute/path/to/mainnet/blocks \
  --expected-magic 0xd1a0c0de \
  --expected-tip "$(dinero-cli -datadir=/absolute/path/to/mainnet \
    getbestblockhash | tr -d '"')"
```

The command exits nonzero if a checksum or canonical deserialization fails, if
the expected tip is absent, or if a revealed CCV opcode is found. Its JSON
result states the Taproot-opacity limitation explicitly.

## Construction-provenance review

The live node and its local wallet state were inspected read-only:

- The running daemon registered zero RPC method names containing `ccv`,
  `covenant`, or `contract`.
- 34 current SQLite wallet/sidecar databases were examined through
  `sqlite_master`; none contained a table whose name contains `covenant` or
  `contract`.
- No current node log contained `CCV`, `covenant`, or
  `CHECKCONTRACTVERIFY`.
- Production source has zero constructions of `CovenantWallet` outside its
  own implementation. The only constructions are in the obsolete integration
  test.
- `src/rpc/methods_wallet_covenant.cpp` is not in the production build, and
  `register_context_wallet_covenant_methods()` remains commented out in
  `src/daemon/rpc_context_wiring.cpp`.

This proves that the shipped production interfaces did not provide the
claimed CCV constructor or state tracker. It cannot exclude a manually crafted
P2TR output whose unrevealed tree was never recorded by a wallet. The operator
controls the three-node deployment and attests that no such manual CCV output
was created; that operational provenance is the final input to the Case A
classification.

## Remaining activation gates

Before choosing a mainnet height:

1. Repeat this read-only audit against a fresh archival snapshot and record the
   exact tip and scanned-prefix digest.
2. Confirm that all operator wallet backups and any server-local wallet stores
   have no external/manual CCV construction records.
3. Port wallet/RPC construction to the v1 state-derived internal key and
   successor-output rules; do not revive the historical builder unchanged.
4. Run activation-boundary, mempool/mining, reorg, restart, and three-node
   mixed-version tests.
5. Obtain independent review of the hash-to-x derivation, previous-output
   binding, successor uniqueness, and fee-input model.
6. Upgrade all validators before setting and shipping an activation height.

Confidential CCV transitions remain unsupported until a separate equality
proof protocol is specified, implemented, and reviewed.
