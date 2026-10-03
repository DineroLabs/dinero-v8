# Orchard commitment frontier

`OrchardFrontier` is an immutable C++ value backed by the pinned upstream
`incrementalmerkletree::frontier::Frontier<MerkleHashOrchard, 32>` in Rust.
It provides empty construction, canonical storage decoding, append and derived
root/size/bytes. It is a component for the block transition, not admission or
proof that the decoded history belongs to the active chain.

## Draft storage format

- Eight magic/version bytes `DNORFR01`.
- Little-endian uint64 leaf count, bounded by 2^32.
- Empty trees end there (16 bytes).
- Nonempty trees contain the last leaf's canonical 32-byte field encoding,
  a one-byte ommer count, and that many canonical 32-byte past-subtree hashes.
- Ommer count must equal popcount(leaf count minus one), at most 32.
- The upstream checked constructor validates the frontier position/depth.

The maximum is 1,073 bytes. Oversize, truncation, wrong version, malformed field
encoding, inconsistent shape and trailing bytes reject. This fits the existing
4,096-byte storage limit. The root is computed from the frontier, never accepted
as a caller-provided cached hash. The future state loader must compare BOTH
the computed root and size to the authenticated stored state record.

Append accepts at most eight commitments per call, matching one bundle's action
bound. Each commitment is canonically decoded before use. A block transition
must process all verified actions in canonical transaction/action order; a larger
block is not restricted to eight total actions. Full-tree append rejects.

Rust decodes into a local tree and writes the bounded C output only after the
entire operation succeeds. Failed partial appends leave both input and output
unchanged. Every entry point catches unwinding; raw-pointer validity, alignment,
lifetimes and non-aliasing remain the C caller's obligations. C++ owns all state
and returns a new value on append. There is no shared mutable tree handle.
Cross-language layout assertions cover field offsets and total ABI size.

## Important invariant

Appending the uncommitted leaf (field value 2) may leave the root unchanged.
The size and canonical frontier still change. An initial C++ fixture incorrectly
expected every append to change the root; the corrected test explicitly checks
this case. Root equality cannot substitute for tree-position accounting or
exact state/undo comparison.

## Executed qualification

- Empty root matches the fixed depth-32 vector in orchard 0.15.5's
  `src/test_vectors/commitment_tree.rs`.
- Seventy successive sizes, including carry boundaries, match a separate full
  level-by-level tree walk using upstream Orchard hashing; every step decodes
  its saved frontier before continuing. No new hash primitive is implemented.
- Truncations, noncanonical leaf/subtree nodes, malformed counts, trailing
  bytes, full 2^32-leaf frontier and overflow rejection are covered.
- C++ checks split/batched append equality, reopen, immutable parents, unchanged
  roots with changed sizes, invalid partial append and preserved FFI output.
- Eight root CTests and six standalone CTests passed, including fourteen Rust
  tests. Rust formatting and clippy with warnings denied passed.
- C++ backend/wrapper ASan and UBSan passed; the Rust archive and OpenSSL are
  not instrumented in that run, and macOS leak detection is disabled.
- A temporary C++ wrapper that passes zero commitments instead of the requested
  list fails the append result test. Production source is not modified for it.

This does not establish witness maintenance, anchor eligibility, block proof
validation, crash recovery or daemon integration. The next block transition
must check the parent frontier against stored state, use verified bundle facts,
enforce anchors/nullifiers and independent pool accounting, then stage the
result with coins, tip and undo in the existing atomic ChainDB batch.

## Header integration repair

The preceding pool-guard commit exposed `tx_validation.h` through the widely
included ChainDB header. Linux builds then stopped in reindexer.cpp because
two existing headers declare MAX_TX_SIZE with different types. ChainDB now
forward-declares the flow value type; the storage implementation includes its
definition directly. The exact reindexer source failed before and compiled after
the repair, and the full local daemon build completed. This changes no constant
or consensus rule. Fresh Linux qualification is still required.
