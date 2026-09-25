# Orchard shared reader: typed parsing before admission

The shared `ParsedTransaction` reader owns either an existing historical
`Transaction` or a staged `TransactionEnvelope`. The variant is immutable and
has no conversion from Orchard to the historical transaction type. Callers must
explicitly select `HistoricalOnly` or `StagedOrchard` parsing. Neither mode is a
consensus activation flag, and parsing does not validate spending authorization.

## Implemented boundary

- The existing historical parser now rejects version 7 followed by two zero
  bytes. That otherwise represents a zero-input, zero-output transaction, not a
  valid ordinary transparent spend. Previously the stream overload consumed ten
  bytes of the Orchard fixture as an empty transaction. A regression failed on
  the old parser (`accepted=1, consumed=10`) and passes with the guard.
- The guard applies without Rust or the backend build option. It runs before
  publishing transaction fields, and failure sets consumed length to zero.
- The typed reader routes that marker family to the bounded Orchard decoder
  only in explicit staged mode. Unknown magic/profile or malformed data cannot
  fall back to historical parsing. Existing RPC/block/storage callers continue
  using the historical parser and therefore continue rejecting Orchard.
- Historical v1/v2/v5/v6, ordinary v7 and the old compact-regtest version retain
  their existing parser and identity functions. The six-version codec fixtures
  test encodings, not the validity of their synthetic signatures or proofs.
- Historical input copying uses the host's existing per-version wire ceiling;
  a prefix read never copies the entire remaining block buffer. Historical
  permissive decoding/canonical reserialization remains unchanged except for
  the invalid empty v7 shape reserved by the marker. No new requirement for
  byte-canonical historical transactions is introduced.
- Exact reads reject trailing bytes; prefix reads consume one transaction and
  preserve the next. Mixed historical/Orchard streams round-trip through the
  typed reader, with the existing strong txid/wtxid types and weight calculation.

## Build and qualification

The root target `dinero_transaction_reader` links the actual consensus-core
primitives. Standalone qualification compiles those same historical transaction
sources, not a replacement parser. Root block tests link the canonical chain
parameter implementation needed by the core block/Utreexo codec; no stub is used.

Native macOS Release: all three standalone CTest entries pass, and all four root
entries pass: `OrchardBackendCpp`, `OrchardBackendRust` (eleven Rust cases),
`OrchardTransactionReader`, and `OrchardLegacyBoundary`. The latter is registered
unconditionally in root builds, including when Orchard is disabled. The root
reader test uses the real `Block::Deserialize`: an ordinary block decodes and a
block carrying the marked Orchard envelope is refused.

The root build uses already-built OpenSSL 3.5.7 artifacts read-only. It rebuilt
the touched core sources and relevant tests; this is not a full-daemon or full
CTest qualification. Standalone C++ ASan/UBSan checks cover the typed reader and
historical parser; Rust is a release archive, and no leak check is claimed.
Linux workflow inventories now require three standalone/four root entries, and
the root job builds the daemon plus all three C++ test executables.

## Next integration gate

Live callers are deliberately not switched to staged mode. Before that switch,
implement the authenticated coin adapter and supported transparent witness rules,
including signing that commits to Orchard effects, then anchors/nullifiers/pool
accounting, atomic state/undo and activation. Existing transparent sighash code
must not be reused under an assumption that it covers the new Orchard effects.
Wallet proving/signing, durable identity finalization and lifecycle tests remain.
No mainnet switch, production database or fleet binary changed in this slice.
