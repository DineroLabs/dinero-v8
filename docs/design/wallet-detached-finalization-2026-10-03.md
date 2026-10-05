# Prepared Orchard finalization with checked publication

Catalog finalization captures and authenticates every enrolled account and reached archive in a checked FULL transaction, then releases wallet, seed and SQLite ownership before full restoration against the immutable replay source. Preparation binds the exact operation, request, wallet identity, current account revision and owned executor result in a noncopyable process-local plan. It creates no replacement account or proof authority.

The consumed writer rechecks the complete catalog and exact executor state/proof. It stages the existing Ready transition and, for shielding, its ordinary history in one FULL transaction. It authenticates the expected post-state and checks COMMIT before retiring the matching executor result or returning Ready bytes. Refused reads, writes or COMMIT preserve the prior durable reservation and proof for a fresh owned retry. A previously copied proof cannot promote a reservation after its executor owner is gone. Matching Ready retries retain their original body and history time.

The actual shield service prepares this work before reacquiring selected-chain ownership. Its existing selected head, profile, coin, resource and authorization checks still precede the durable Ready/history commit. Prepared bytes alone do not authorize admission, relay or mining.

## Tests and remaining scope

Seven component cases cover released restoration, exact proof retirement after commit, required read/write/COMMIT refusal, whole-catalog changes and late authentication failures, foreign/caller ownership and source refusal, missing executor work, exact Ready revision/retry behavior, and the selected shield-history boundary. Registration remains enabled with a 180-second deadline. Actual qualification results are recorded separately; this document claims no executed test result.

Shield signing and reservation writers retain their separate remaining ownership work. This change does not establish cross-process or backup rollback protection, broad platform/load qualification, or release readiness. Mainnet activation remains unset.
