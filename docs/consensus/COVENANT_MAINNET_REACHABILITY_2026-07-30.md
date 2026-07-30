# Covenant mainnet reachability audit — 2026-07-30

## Result

The canonical mainnet chain through height 75,490 contains no Taproot
script-path spend and no revealed or bare covenant opcode. Every observed P2TR
spend is a key-path spend.

This supports the operator's statement that the covenant features were never
used. It does not prove that the 71,203 unspent P2TR outputs have no hidden
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
5. parses script push operations so opcode-valued bytes inside pushed data are
   not false positives.

| Field | Result |
|---|---:|
| Network | mainnet |
| End height | 75,490 |
| End hash | `000000856e22c706820cde87e5d4ed2d32db3f8a1a920ed02c98fa8511038595` |
| Canonical blocks | 75,491 |
| Transactions | 75,567 |
| Inputs | 77,843 |
| Outputs | 228,557 |
| P2TR outputs created | 73,555 |
| P2TR outputs spent | 2,352 |
| P2TR key-path spends | 2,352 |
| P2TR script-path spends | **0** |
| P2TR malformed spends | 0 |
| P2TR annex spends | 0 |
| Unspent P2TR outputs | 71,203 |
| Revealed covenant opcodes | **0** |
| Bare covenant opcodes | **0** |

The first P2TR output is at height 1. The first P2TR key-path spend is at
height 7,187.

The generated JSON result was 31,005 bytes with SHA-256:

`42cb9162bdd529483a0620e040250c1fee461b0d41c6a0eea79c675d045e97c0`

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
