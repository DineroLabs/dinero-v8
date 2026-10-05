# Detached authenticated catalog reads

`ReadCatalogForReplay` captures the complete declared account inventory under an existing wallet identity and one checked FULL SQLite transaction. It owns authenticated current account and reached archive bytes, all retained account and archive payloads, the catalog, an opaque database instance token and derived viewing keys. Recovery seed pins and database leases do not escape capture. Generated-empty catalogs remain distinct from unknown recovery state and do not create schema.

The standalone read releases its own transaction and wallet ownership before restoring current scans and linked retained parents against the immutable replay view. It preserves the existing exact account encoding check and archive relationships. A second authenticated capture rechecks the catalog, instance, identity, every captured revision and plaintext payload, and viewing keys. Current, reached-archive and linked-parent shield histories are authenticated in both captures. The result returns only after checked commit; any final read, mismatch or commit refusal returns no catalog.

Captured plaintext uses the existing cleansing byte owner; viewing keys use scoped cleansing. Retained archive revisions are indexed once before capture. Existing row/state limits still apply; retaining a complete capture is not a general-history resident-memory bound.

The private point-provider implementation is shared with the production entry point, which always obtains points from the supplied immutable replay view. A narrow friend fixture wraps real lookups to observe zero same-thread wallet leases and seed pins and SQLite autocommit during funded restoration. Five cases cover nonconsecutive accounts, final read and commit refusal, generated-empty and borrowed-transaction refusal, restoration ownership, and reached/retained archive authentication and exact revision rechecks. The existing operation-status interrupted-read fixture targets the shared inventory query's actual ordering; its refusal and no-prefix assertions remain intact.

## Scope and remaining work

This is an as-of authenticated read, not enduring spend authorization. Callers that retain an outer lease still retain it. Transaction-owned catalog readers and writers remain separate paths and must be migrated before claiming complete caller detachment or permitting lazy proof verification there. Selected-source rechecks, whole-node qualification, general-history capacity, platform/load and release readiness remain required. Mainnet activation remains unset.
