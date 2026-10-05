# Prepared account creation and receiver issuance

Generated-catalog account creation and receiver issuance capture all existing account, archive, retained snapshot and ordinary shield-history owners in a checked FULL SQLite transaction. They bind the wallet session, persistent identity and derived viewing key before releasing wallet, seed and database ownership. Capturing an empty generated catalog does not initialize the account schema or invent an owner.

Full account restoration and immutable replay lookups happen after those owners are released. Creation derives the requested account at the authenticated replay origin; issuance advances the existing account's receiver counter. The prepared address stays private. Both entrypoints refuse a borrowed caller wallet lease or SQLite transaction, so a caller cannot silently keep ownership across this released phase.

The writer reacquires ownership and compares the complete authenticated catalog with the captured bytes before effects. It checks the same session, persistent identity and viewing authority. Issuance retains the previous account snapshot; creation writes the first snapshot and appends the account to the catalog in the same transaction. Only this checked creation writer can initialize the first account schema.

Before COMMIT, the writer authenticates the complete resulting catalog. It checks the exact expected new account or retained prior snapshot, every unchanged account and archive, retained row counts and ordinary shield histories. It returns the prepared address only after checked COMMIT. Replay lookups and full account restoration do not run under the writer. A refusal returns no address and rolls back all changes in this transaction.

## Qualification and limits

Five component cases cover empty creation and funded issuance with released owners; capture/write COMMIT and final-read refusals; catalog or other-account changes during preparation; caller ownership, locked-wallet and source refusal; and exact archived operation ciphertext preservation. The three preexisting catalog-issuance cases retain their assertion bodies. Their SQL interrupt matcher follows the actual complete inventory query. Execution results are recorded separately; this document does not claim these tests passed.

The generated catalog remains required. Narrow legacy issuance and other account mutation/proof paths retain separate contracts. This change does not establish multiprocess consistency, backup rollback detection, a single transaction across databases, general-history memory bounds, whole-node/platform/load qualification or release readiness. Mainnet activation remains unset.
