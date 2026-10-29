# Detached Orchard spend reservation

The spend-reservation owner captures the complete present authenticated catalog in a checked FULL transaction and releases wallet, seed and SQLite ownership before restoring accounts from immutable replay points. It retains account, archive and ordinary-payment ownership from that capture for the later writer recheck.

An exact stored request retry is authenticated against the captured pending operation or reached archive. It keeps the original request identity and result without preparing a proof, selecting notes again or reconciling archived operations. New work computes required catalog reconciliation outside the writer while retaining the existing note-selection, change and signing-context rules.

One sole FULL transaction rechecks the entire captured catalog, applies every required archive reactivation and reserves the new operation. A later capacity, write or commit refusal rolls back the complete group. The proof submission becomes publishable only after the transaction commits and wallet ownership is released. A signed or reserved operation alone is not evidence of admission, relay, mining or lasting selected-chain validity.

## Qualification boundary

Six enabled component cases cover restoration after ownership release, proof publication after commit, capture/recheck/write/commit refusal, changes to another account, borrowed ownership/locked wallet/source refusal, exact retry with a stopped executor, and rollback of another account's actual archive reactivation on capacity refusal. The CTest deadline remains 180 seconds and its future Actions lane requires all six successful case markers and retains inventory and execution logs. This document claims no executed test result.

This packet depends on the preceding ordinary-payment packet and preserves its registration. Both candidates still require fresh qualification before installation. Shield reservation has a separate candidate. Complete backup/deletion protection, broad platform/load, configured consumer behavior and release qualification remain open. Mainnet activation remains unset.
