# Shielded migration metadata: UTXO schema 2

The real daemon's UTXOIndex creates schema 2 with explicit wallet coin ownership columns. Its utxo_metadata key/value table and the consensus/recovery metadata interpreted by the offline shielded migration reader retain the schema-1 format. The previous inspector rejected an actual stopped daemon datadir before migration because it required user_version=1.

The reader now recognizes versions 1 and 2 explicitly. Unknown versions remain refused. All immutable read, type, terminal-read, resource, pending recovery, AssumeUTXO, protected history and nullifier-cache checks remain in place. No SQLite data or version is rewritten by migration; the outer lease and complete companion byte inventory continue to protect the stopped original and candidate. This format check is not authentication of wallet ownership or a completeness certificate.

Two explicit metadata-only CTests execute the same existing healthy/refusal cases for both versions. The future-version fixture uses 3 now that 2 is supported; every existing refusal assertion is retained. These tests exercise synthetic companion metadata and chain fixtures. The separate real-daemon HTTP migration and wallet cycle remain required. The complete cohort suite is not substituted for the selected safe cases.

Qualification is pending for this source. Production upgrade/cutover, fresh-layout initialization, Orchard full reindex, complete wallet cycle, platform/load and release gates remain open. Mainnet activation remains unset; no deployment authorization.
