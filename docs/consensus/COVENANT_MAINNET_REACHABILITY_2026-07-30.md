# Covenant mainnet reachability audit — 2026-07-30

## Result

The canonical mainnet chain through height 76,105 contains no Taproot
script-path spend and no revealed or bare covenant opcode. Every observed P2TR
spend is a key-path spend using a 64-byte signature with implicit
`SIGHASH_DEFAULT`.

This supports the operator's statement that the covenant features were never
used. It does not prove that the 71,818 unspent P2TR outputs have no hidden
script tree. Taproot intentionally hides scripts until they are spent.
Consequently, this audit permits a coordinated covenant activation plan; it
does not justify silently changing the meaning of every historical P2TR output.

## Canonical-chain evidence

The scan was pinned to the RPC-reported mainnet tip and then performed offline.
The scanner:

1. verifies the FNV-1a checksum on every `blkNNNNN.dat` record;
2. computes each block hash and walks backward from the pinned tip, excluding
   stale and orphan records;
3. mirrors Dinero's transparent, confidential, witness, and shielded
   transaction serialization to recover txids and follow P2TR outpoints;
4. applies the BIP341 trailing-annex rule before classifying key-path and
   script-path spends; and
5. records every key-path signature length and explicit sighash byte, so
   corrected non-default sighash behavior cannot silently reinterpret history;
   and
6. parses script push operations so opcode-valued bytes inside pushed data are
   not false positives.

| Field | Result |
|---|---:|
| Network | mainnet |
| End height | 76,105 |
| End hash | `000000597d60ede4a0afeed16c01ec9408f33e9fc7956cc8b9d9e06f3ed61f51` |
| Canonical blocks | 76,106 |
| Transactions | 76,182 |
| Inputs | 78,458 |
| Outputs | 230,402 |
| P2TR outputs created | 74,170 |
| P2TR outputs spent | 2,352 |
| P2TR key-path spends | 2,352 |
| 64-byte implicit `SIGHASH_DEFAULT` spends | **2,352** |
| 65-byte explicit-sighash spends | **0** |
| P2TR script-path spends | **0** |
| P2TR malformed spends | 0 |
| P2TR annex spends | 0 |
| Unspent P2TR outputs | 71,818 |
| Revealed covenant opcodes | **0** |
| Bare covenant opcodes | **0** |

The first P2TR output is at height 1. The first P2TR key-path spend is at
height 7,187.

The schema-v2 generated JSON result was 34,108 bytes with SHA-256:

`f8b20d2f0ea620dd6ff7613e828743de1cc072ae84eaa1cae0d5f962f2617367`

## Reproduction

Run against a synchronized, unpruned mainnet node:

```sh
node scripts/audit/covenant_history_scan.js \
  --cookie "/absolute/path/to/mainnet/.cookie" \
  --rpc-port 20998 \
  --blocks-dir "/absolute/path/to/mainnet/blocks" \
  --output "/new/path/covenant-history.json"
```

The output path must not already exist. The command refuses to label a
non-mainnet scan as mainnet, pins its end height/hash before reading block
files, verifies every record checksum, proves the pinned tip is present, and
requires the backward canonical walk to contain exactly `height + 1` blocks.

## Consensus implication

There is no historical execution that corrected rules must replay. However,
unspent P2TR outputs are opaque. Corrected covenant semantics therefore need:

- an authoritative, checksummed activation parameter;
- a clearly versioned post-activation interpretation;
- fail-closed behavior for incomplete covenant operations;
- boundary, reorg, mempool, mining, restart, and mixed-version tests; and
- coordinated validator deployment plus independent consensus review.

Operator provenance—that the controlled miners and wallets never manually
constructed hidden covenant leaves—is necessary additional evidence, not a
fact derivable from the chain.
