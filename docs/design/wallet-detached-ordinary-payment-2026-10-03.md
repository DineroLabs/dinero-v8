# Ordinary payment preparation and commit

The ordinary-payment path captures the complete present authenticated Orchard catalog under a checked FULL transaction, then releases wallet, seed and SQLite ownership before restoring account state and computing archive reconciliation against immutable replay points. The actual service prepares before taking selected-chain ownership and retains its existing selected-parent and safe-mode checks.

The consumed payment plan opens one sole FULL writer and rechecks the complete captured catalog. Every required archive reactivation and the ordinary payment belong to this same transaction. The writer preserves both ordinary and Orchard input-reservation checks and the existing transaction signing/history owner. It releases the signing seed, authenticates the complete expected post-state and commits before returning the payment result. A later signing or commit refusal rolls back archive reactivation with the payment rather than leaving a separately committed reservation prefix.

No account, path, key, fee, history category, cursor or readiness state is invented. The method does not claim lasting selected-chain validity or successful admission, relay or mining from a signed result alone. Existing exact transaction/history identity and service admission obligations remain separate.

## Tests and remaining scope

Six component cases cover restoration after ownership release, selected commit, capture/read/history/commit refusal, changed account/session, borrowed ownership/lock/source refusal, both reservation sets, and actual archival operation reactivation followed by signing refusal. Registration is enabled with a 180-second deadline. All nine existing shared-reservation cases remain in the separate qualification recipe alongside prior signing/finalization/recovery cases. This document claims no executed test result.

Orchard spend and shield reservation candidates require their own qualification. Complete backup/deletion protection, configured consumer/provider behavior, selected-parent activation, broad platform/load and release qualification remain open. Mainnet activation remains unset.
