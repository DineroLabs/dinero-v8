# Draft Orchard envelope, profile 1

This is an implementation candidate on the integration branch, not a
frozen consensus protocol. No new bytes are routed into production admission.
Existing transparent v7 and historical v5/v6 serialization remain untouched.
The shared typed reader is implemented, while legacy callers explicitly reject
the marked family until full admission and activation are wired.

## Exact framing

All counts/lengths below are unsigned, fixed-width little-endian u32. Amounts
are u64. The serialized size, including framing, may not exceed 100,000 bytes.

| Field | Encoding |
| --- | --- |
| Numeric version | u32 7 |
| Disjoint marker | 00 00 |
| Magic | ASCII DNORCHTX |
| Outer profile | byte 01; same wire-profile constant used by inner codec and signing D |
| Payload size | u32, exact following byte count |
| Inputs | u32 count; for each: txid wire bytes32, vout u32, scriptSig blob, sequence u32, witness-item count u32, each item blob |
| Outputs | u32 count; for each: amount u64, scriptPubKey blob |
| Explicit fee | byte 01, amount u64; absent fee forbidden |
| Orchard bundle | blob containing exact inner DNORCH01 encoding |
| Locktime | u32 |

A blob is u32 byte length followed by exactly that many bytes. There is no
alternate compact-integer encoding. Limit inputs/outputs to 4096 each, scripts
and witness items to 10000 bytes, witness items to 100 per input and 4096 total,
and inner bundle to 65536 bytes. Every scriptSig must be empty, in both the
builder and decoder. Profile 1 is intended for native witness inputs; legacy
and nested-witness scriptSig authorization are not supported. Output sum plus fee must fit MAX_MONEY; duplicate
inputs and coinbase outpoints are forbidden. The decoder validates framing and
bounds before allocating each vector. The builder checks aggregate bounds before
copying nested input. Memory use includes object overhead and owned copies, so
the serialized-byte bound is not a claim of 100,000-byte total RAM use.
The component compiles assertions against the host general transaction, block
and weight limits even when tests are disabled. No legacy limit is raised.

DecodeExact rejects trailing bytes. DecodePrefix consumes only the declared
bounded frame and returns its consumed length for block streams. Extra bytes
inside a frame reject. Neither method falls back to a generic transparent parser.

## Identities and authorization

The candidate txid is double SHA256 of the same frame with every transparent
witness count replaced by zero and witness items omitted; payload length is
recomputed. The empty scriptSig length fields and the COMPLETE Orchard bundle, including all proof and
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

Transparent witnesses are excluded from candidate D and require their own
validation. A nonempty scriptSig is structurally rejected, eliminating an
unsigned mutable field from txid. Host admission MUST resolve the previous
output, require a supported native witness program, reject unexpected witness
and enforce the complete script/stack rules before producing any full-validation
result or populating its cache. The current component performs none of those
script checks, and its synthetic signing fixture is not a valid transparent spend.
Multiple wtxids for one txid are possible by design; wire bounds limit each
candidate but do not establish a cache policy. The host must bound admission,
deduplication and caches; neither identifier alone substitutes for validation.

The outer profile is an alias of the same wire-profile constant already present
in D and checked against Rust. It cannot be selected independently; an unknown
header profile rejects. A future outer format requires a joint protocol-profile
change and new signing vectors, not reuse of the current signature domain.

Orchard proof/signature changes leave D unchanged but change both identities.
Corrupt authorizations reject; a newly generated valid randomized proof can
change identity. Wallets must finalize proofs and signatures and persist the
exact transaction before exposing its txid or constructing child transactions.
A relayer changing a parent's identity invalidates an existing child's reference;
it does not by itself destroy the parent's output, which can be spent by its
actual outpoint. Ciphertext changes affect D itself.

## Evidence and remaining gates

The Python vector generator only encodes the public synthetic fixture; it does
not generate proofs, access wallets or contact a node. It produces bytes/txid/
wtxid independently of the C++ implementation. C++ validates round trips,
bounded/truncated inputs (the current fixture has 9346 truncated prefixes),
stream consumption, fixed fee marker, matched coins and the original signature.
Proof corruption rejects through the envelope API.

The prefix is a candidate from the disjoint-marker design contract. This does
not establish rejection by every old binary path, especially historical reindex.
Actual old/new parser/admission/storage/compact-block compatibility, cross-node
regtest activation, new proof construction, wallet flows, and final protocol
review remain open. One-sided flow encoding tests are not valid-proof tests.
No historical transaction is reinterpreted or production activation selected.

## Pinned signature-encoding review

`Cargo.lock` selects reddsa 0.5.2 and pasta_curves 0.5.2. Orchard's RedPallas
wrapper calls reddsa `VerificationKey::verify`; `verify_prehashed` decodes R with
`GroupEncoding::from_bytes` and S with `PrimeField::from_repr`, rejecting failed
conversions before the signature equation. Pasta's compressed point decoder uses
a canonical field representation plus the encoded sign; its scalar decoder
rejects values at or above the modulus instead of reducing them.
`Signature::from` alone is only a byte container, not a verification result.
The component always verifies both spend and binding signatures. Four bounded
reject-only cases (noncanonical R/S for each role) pass alongside the valid
fixture. This is source-backed encoding evidence, not a replacement for the
independent cryptographic review or an assertion of unique randomized signatures.

## Shared host reader

See [typed parsing boundary](../../docs/design/orchard-shared-reader-2026-09-24.md).
`ParsedTransaction` preserves the variant and strong identity types. Calling
`Historical()` on an Orchard result throws instead of fabricating a transparent
transaction. The root block decoder test verifies that enabling the component
still does not admit Orchard blocks through current callers.
