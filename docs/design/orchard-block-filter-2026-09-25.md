# Draft Orchard compact-filter integration

## Pre-commit validation

The full staged Orchard chainstate connector computes the GCS compact filter
from the exact validated mixed transaction coin plan. It includes nonempty
spent scripts (including same-block spends) and created scripts except
OP_RETURN, deduplicated by the existing GCS builder. The key remains the parent
hash, avoiding a circular dependency with the coinbase commitment.

The existing DNRF commitment rules apply to the new profile before any durable
write. Missing or incorrect required commitments reject the candidate and the
outer batch remains empty. An empty filter still requires a commitment to its
zero hash; it is not a reason to skip validation. The shared historical builder
currently returns an empty script for a zero hash, so the eventual Orchard miner
must explicitly encode that case. Historical validation paths are unchanged.

The helper checks that the sealed coin plan belongs to the exact candidate;
transaction identities and ordering cannot be substituted. The coin-only and
Orchard-only staging functions remain partial validators. Only the combined
chainstate connector enforces this additional full-block obligation.

## Durable state and recovery

The verified filter bytes and element count share the same ChainDB batch as
coins, pool, nullifiers, frontier, forest delta, body, transaction indexes and
tip. A retained filter for this block hash must match before reconnect. Filters
remain by block hash after disconnect for archival serving and reconnect.

Startup audit/disconnect reconstructs the scripts from the authenticated body
and checked undo, including same-block inputs. It compares both bytes and count,
then checks the commitment again. Comparing only the encoded-data hash would
miss corrupt element-count metadata, since that count is stored separately.
Failures in local persisted data are lookup/corruption failures, not a reason to
mark a peer's header consensus-invalid.

## Qualification and remaining integration

Tests cover mixed-family scripts, same-block inputs, candidate binding,
missing/mismatched and empty commitments, atomic abandonment/reopen, count
corruption, disconnect/reconnect and process-exit boundaries. Actual daemon
admission, miner generation, mobile filter serving and postactivation runtime
routing still need integration. This is not an activated consensus change or
complete lifecycle/release qualification.
