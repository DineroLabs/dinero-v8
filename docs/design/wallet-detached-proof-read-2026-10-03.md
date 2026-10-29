# Account proof reads with released restoration ownership

The three catalog proof-reading entrypoints capture the complete generated account catalog under one checked FULL SQLite transaction, binding the current wallet session, persistent identity and derived viewing authority. They release wallet, seed and database ownership before fully restoring every captured account against the immutable replay. Callers retaining a wallet lease or SQL transaction refuse.

The requested operation must exist. Spend and shield request variants authenticate the exact ordered request commitment against the captured wallet/domain/account identity. A second checked FULL transaction reauthenticates the complete captured catalog, then copies the matching executor state/proof through the existing ownership check. COMMIT must succeed before a result is returned. Missing executor work remains an explicit missing state; it does not fabricate a proof or change the durable reservation.

These reads do not write account state, retire a proof job, release capacity or reservations, recreate keys/accounts, or publish a readiness flag. The returned proof is a copy valid for the observed owner; selected-chain admission and any later durable transition must perform their own checks.

## Tests and remaining scope

Five component cases cover queued/missing/completed results, exact spend and shield request binding, capture/final-read/COMMIT refusal, changed other accounts, locked or foreign wallet owners, borrowed caller ownership and source refusal. Their actual results are recorded separately. This source document claims no executed qualification.

Finalization, signing, reservation writers and other operations retain their separate contracts. This change does not establish cross-process consistency, backup rollback detection, whole-node/platform/load qualification or release readiness. Mainnet activation remains unset.
