# Detached shield reservation

The three service shield-reservation paths prepare the complete present authenticated account catalog before taking selected-chain ownership. Account restoration and archive reconciliation use immutable replay points after wallet, seed and SQLite ownership are released. Selected-chain checks retain their existing scope and recheck the prepared network, genesis, branch and activation profile.

Exact stored request retries preserve the original request inputs, outputs, fee and operation identity. They precede current coin availability and do not generate another plan, select replacement inputs, issue change or require a running proof executor. New automatic selection reads ordinary coin candidates and existing signing owners after a complete captured-catalog recheck, then filters captured Orchard reservations.

The reservation writer rechecks the entire catalog, performs all required archive reactivations and checks both ordinary and Orchard reservations in one sole FULL transaction. It validates unreserved inputs and existing change signing owners, reserves the request and binds an unpublished proof submission before the final catalog recheck and COMMIT. The service releases selected-chain ownership before publishing that submission.

Automatic change issuance retains its existing separate transaction. A later reservation failure can leave an unused issued change address; this change does not establish whole-RPC atomicity. It does not create substitute keys, paths, accounts, fees, labels or readiness.

## Qualification boundary

Seven enabled component cases cover released restoration and selected commit, capture/writer/commit refusal, catalog and reservation changes, borrowed ownership/locked wallet/source refusal, stored retry and captured candidate reads, automatic selection and exact retry, and actual archive reactivation rolled back with a conflicting new input. The CTest uses the existing 180-second deadline. Its future Actions lane requires seven successful case markers and preserves inventory and execution logs. This document claims no executed test result.

The ordinary-payment and spend-reservation packets are prerequisites and their registrations are preserved. Every candidate requires fresh qualification before installation. Admission, relay, mining, complete backup/deletion protection, broad platform/load, configured consumer behavior and release qualification remain separate gates. Mainnet activation remains unset.
