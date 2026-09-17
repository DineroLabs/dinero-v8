# Compact regtest package admission

This qualification adds test tooling only. It does not change package policy,
consensus validation, Utreexo behavior, production activation, or the compact
encoding. It requires the default-off `DINERO_ENABLE_COMPACT_REGTEST` build.

## What runs

`CompactPackageBoundaries` starts real isolated, offline regtest daemons. It
mines a funding chain, creates a real compact unshield through the wallet RPC,
and builds a separate compact shield with 30 transparent outputs. The latter
uses the production proof builder; the daemon wallet signs its transparent
input. Every admission goes through both canonical `mempool.testmempoolaccept`
and, where indicated, `wallet.sendrawtransaction`. No admission bypass, fake
proof, synthetic mempool insertion, or external seed data is used.

Each case clones the same cleanly stopped seed, requires identical canonical
state after restart, and starts from the same restored unshield. Exact wire
sizes use signed transactions padded with positive-value OP_RETURN outputs
whose scripts are bounded to 80 bytes. A separate Python byte reader computes
the transaction ID and requires the daemon to agree.

| Case | Required outcome |
| --- | --- |
| Shielded ancestor package | 599,999 accepted by dry run; 600,001 rejected by both paths; 600,000 submitted successfully |
| Transparent-only ancestor package | Same checks at 103,423 / 103,425 / 103,424 bytes |
| Shielded direct-parent descendant set | Same checks at 599,999 / 600,001 / 600,000 bytes |
| Transparent-only direct-parent descendant set | Same checks at 103,423 / 103,425 / 103,424 bytes |
| Two branches sharing a compact ancestor | Shared ancestor counted once; exact 600,000 accepted and 600,001 rejected |
| Ancestor count | 25 ancestors excluding candidate admitted; 26 rejected |
| Descendant count | 25 descendants excluding parent admitted; 26 rejected |
| Transparent transaction with shielded ancestor | 99,999 dry-run accepted; 100,001 rejected; 100,000 admitted |
| Invalid controls | Corrupt transparent signature and compact output proof rejected; original valid transactions still admitted afterward |

The transparent-only cases retain an **unrelated** compact unshield in the
mempool. Its presence must not enlarge their package budget. Every byte/count
rejection requires the specific expected policy code, so an unrelated
signature, fee, maturity, proof, or funding failure cannot satisfy the test.

The existing counting convention is intentionally preserved: ancestor bytes
include the candidate and unique ancestors; ancestor count excludes the
candidate. Descendant accounting considers the complete descendant set for
each direct parent, includes the candidate, and excludes that parent. This
test does not silently redefine the policy as a 25-transaction total package.

## State invariants

Every dry run must leave mempool membership and canonical state unchanged.
Real rejection must also leave both unchanged. Real acceptance must insert
exactly the independently identified transaction, while canonical chain state
remains unchanged. The state comparison includes:

- Best block hash and the full Utreexo commitment RPC result.
- `daemon.shieldedstatehash`, covering the canonical Utreexo forest, shielded
  commitment tree, nullifier set, and anchor history.
- Sorted mempool transaction IDs, without connection counts or timing fields.

For byte boundaries, the same input is subsequently admitted at the exact
limit. The invalid controls similarly admit the original valid transaction
after its corrupted counterpart is rejected. These checks also catch an
erroneous input reservation that would prevent the subsequent valid spend.

## Fixture transport details

The current `wallet.signrawtransaction` RPC accepts a floating-point DIN
prevout amount and truncates `double * 1e8`. The fixture explicitly checks that
this conversion produces the exact integer una amount, using the next
representable double if necessary. This does not change transaction values or
relax signature validation. The underlying RPC conversion remains separate
follow-up work: a reproduced prevout of 998,956,698 una otherwise became
998,956,697 una in the signing context and produced an invalid signature.

RPC requests are paced below the daemon's transport rate limit. Only explicit
HTTP 429 responses, returned before dispatch, are retried. Validation errors
and ambiguous socket failures are never retried into a passing result.

## Execution and limits

Build `dinerod` and `compact_package_builder` in a Release build with
`ENABLE_TESTS=ON`, `ENABLE_ZK=ON`, and `DINERO_ENABLE_COMPACT_REGTEST=ON`, then:

```sh
COMPACT_PACKAGE_EVIDENCE_DIR="$PWD/evidence/packages" \
  ctest --test-dir build-compact-regtest --no-tests=error --output-on-failure \
  -R '^CompactPackageBoundaries$'
```

The `Compact package admission` Linux workflow builds both executables,
records their hashes and source commit, retains per-daemon logs and structured
case receipts, and runs this CTest entry. The mandatory-execution checker
credits its lane alongside the existing compact lifecycle and fixed-vector
lanes. No baseline exemption is added; the test is absent from default-off
builds and required in the experimental inventory.

This gate qualifies package admission. It does not establish pool-protocol
compatibility, production activation safety, or worst-case performance under
adversarial load. Mining, reorg, historical proofs, restart and reindex remain
covered by the separate compact lifecycle and recovery gates.

## Local qualification

The macOS ARM64 Release CTest entry passed all nine scenarios: 100 canonical
dry runs and 92 real submissions, including ten expected rejections through
both paths. Corrupting the output proof reached `shielded validation failed:
proof-invalid`; the rejection was not caused by a stale transparent signature.
All canonical state and membership assertions passed.

The experimental execution inventory reports four registered tests and four
executing tests, with no omissions. As a negative control, removing the new
workflow from that checker makes it fail specifically on
`CompactPackageBoundaries`. Linux execution remains a separate qualification
result, not something inferred from the local pass.
