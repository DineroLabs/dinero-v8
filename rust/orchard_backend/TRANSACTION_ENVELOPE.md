# Draft Orchard envelope, profile 1

This is an implementation candidate on the private integration branch, not a
frozen consensus protocol. No new bytes are routed into production admission.
Existing transparent v7 and historical v5/v6 serialization remain untouched.

## Exact framing

All counts/lengths below are unsigned, fixed-width little-endian u32. Amounts
are u64. The serialized size, including framing, may not exceed 1,048,576 bytes.

| Field | Encoding |
| --- | --- |
| Numeric version | u32 7 |
| Disjoint marker | 00 00 |
| Magic | ASCII DNORCHTX |
| Outer profile | byte 01 |
| Payload size | u32, exact following byte count |
| Inputs | u32 count; for each: txid wire bytes32, vout u32, scriptSig blob, sequence u32, witness-item count u32, each item blob |
| Outputs | u32 count; for each: amount u64, scriptPubKey blob |
| Explicit fee | byte 01, amount u64; absent fee forbidden |
| Orchard bundle | blob containing exact inner DNORCH01 encoding |
| Locktime | u32 |

A blob is u32 byte length followed by exactly that many bytes. There is no
alternate compact-integer encoding. Limit inputs/outputs to 4096 each, scripts
and witness items to 10000 bytes, witness items to 100 per input and 4096 total,
and inner bundle to 65536 bytes. Output sum plus fee must fit MAX_MONEY; duplicate
inputs and coinbase outpoints are forbidden. The decoder validates framing and
bounds before allocating each vector. The builder checks aggregate bounds before
copying nested input. Memory use includes object overhead and owned copies, so
the serialized-byte bound is not a claim of one-MiB total RAM use.

DecodeExact rejects trailing bytes. DecodePrefix consumes only the declared
bounded frame and returns its consumed length for block streams. Extra bytes
inside a frame reject. Neither method falls back to a generic transparent parser.

## Identities and authorization

The candidate txid is double SHA256 of the same frame with every transparent
witness count replaced by zero and witness items omitted; payload length is
recomputed. ScriptSig and the COMPLETE Orchard bundle, including all proof and
signature bytes, remain included. Wtxid is double SHA256 of the full canonical
frame. Hashes are raw wire bytes, not display-reversed hex.

The Orchard signer digest remains the previously reviewed candidate-D layout.
Its context is derived from this envelope's owned input outpoints/sequences,
outputs, locktime and mandatory fee plus the SAME parsed bundle effect.
Previous outputs must be supplied in input order with matching outpoints; wrong
count/order/outpoints reject. Money and script size bounds precede copies.
This correspondence check is not evidence of unspentness or script authorization.
The host still must resolve coins in a coherent authenticated view, enforce
maturity and scripts, select the consensus domain/branch, and check anchors,
nullifier membership, pool supply and activation.

Transparent scriptSig/witness bytes are excluded from candidate D intentionally
and require their own validation. They remain in txid/wtxid as specified above.
Orchard proof/signature changes leave D unchanged but change both identities and
must fail cryptographic verification. Ciphertext changes affect D itself.

## Evidence and remaining gates

The Python vector generator only encodes the public synthetic fixture; it does
not generate proofs, access wallets or contact a node. It produces bytes/txid/
wtxid independently of the C++ implementation. C++ validates round trips,
bounded/truncated inputs (the current fixture has 9348 truncated prefixes),
stream consumption, fixed fee marker, matched coins and the original signature.
Proof corruption rejects through the envelope API.

The prefix is a candidate from the private disjoint-marker contract. This does
not establish rejection by every old binary path, especially historical reindex.
Actual old/new parser/admission/storage/compact-block compatibility, cross-node
regtest activation, new proof construction, wallet flows, and final protocol
review remain open. One-sided flow encoding tests are not valid-proof tests.
No historical transaction is reinterpreted or production activation selected.
