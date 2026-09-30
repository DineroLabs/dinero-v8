# Explicit retained wallet payment requests

PendingPaymentIntent may carry an explicit VaultWithdrawal domain, nonzero owner and request identities, fee hint, maximum fee and audit context. These are authenticated in the existing seed-derived, persistent-wallet-bound payment envelope along with exact recipients, label, signed body, fee, history and reservations. Supplying an identity is not evidence of genuine vault initialization or authorization.

DNPP03 is selected only when a request exists. Existing DNPP01/02 records remain readable and their no-request encoding is unchanged. A mixed envelope retains the original records and their exact body, intent and times. Every request identity must be unique; changing any request payload for an existing identity refuses. The requested maximum fee is checked against the actual signed fee.

FindRetainedWalletPayment rechecks the selected session, pins its seed, fully authenticates the owner/history inside a checked transaction and returns the existing immutable origin. Missing differs from unavailable or malformed storage. It never selects coins, signs, writes or submits. Actual SignAndStageWalletPayment refuses an already-retained request before per-input signing-key lookup and rechecks uniqueness in the write transaction; callers resolve a retry through the lookup API. Existing untagged payment behavior remains unchanged.

The existing wallet.listpendingpayments handler exposes the authenticated request identity and payload for request-bearing records. Untagged historical rows retain their prior response fields. The result remains a retained-intent report, with no admission or vault-authorization claim.

Scope: this is the actual wallet owner and signing boundary. Vault runtime callbacks and the old sidecar are not converted by this batch. It does not establish vault initialization, durable vault state, admission, broadcast, readmission, chain confirmation or cancellation. A retained body is not automatic permission to rebroadcast. A future bridge must resolve before selection, retain cross-store prefixes, pass integer amounts, bind the real vault owner and preserve uncertain external outcomes. Missing/deleted whole payment catalogs and backup rollback remain separate completeness work.

New patched-path cases cover mixed single/batch/request records, exact lookup and reopen, changed payload refusal, existing-body signing refusal, checked SQL/write/commit/end-of-read behavior, malformed identities and fee bounds, and locked/stale sessions. No unsafe-original or race/deadlock controls. Qualification results belong to the private evidence receipt, not this design note.

Locally qualified.

## Retained CI churn diagnostics

The completed staged-vault exact CI run failed at Node A bootstrap with blank RPC details and no uploaded inner transcript. The root Orchard workflow now collects the same bounded churn diagnostics as the full Tests workflow. Both collectors include nested mining cycle logs and already-preserved node logs, as well as existing CSN logs/receipts, with the existing 20 MiB per-file tail bound. Wallet/database/cookie files are excluded. Collection runs with always(), and the root artifact includes the resulting directory. Existing CTest commands, case deadlines, assertions, and failure propagation remain unchanged. Extracted collection scripts were checked using synthetic files, including exact truncation and excluded files; no daemon/churn/generation barrier reproduction was run. This adds diagnostics, not a fix or waiver of the failed CI gate.
