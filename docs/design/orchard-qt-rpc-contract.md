# dinero-qt Orchard RPC contract

This is an unqualified desktop integration candidate. `DIN_ENABLE_ORCHARD_UI` remains off by default. The node already implements wallet binding, balances, activation status, structured proof errors and completion by stored request ID. Account discovery and completion-method discovery are implemented in the locally qualified backend. The predecessor desktop candidate passed its selected passive ON/OFF tests; the incoming-history changes here have not been compiled or executed. Actual backend/UI composition and platform qualification remain required. No mainnet activation is scheduled by this work.

## Selection and accounts

`wallet.orchard.getwalletbinding {wallet_name}` returns the selected wallet's binding. Every other `wallet.orchard.*` call carries `wallet_binding`; the node checks it under its actual wallet owner. The UI also checks the wallet, connection and selection generation before dispatch and before consuming replies. A changed endpoint, credentials or datadir revokes the old connection context. This local context token is not a substitute for node-side binding.

The implemented `wallet.orchard.listaccounts {}` command uses the same complete authenticated catalog restoration as balance/operation reads. Its result contains `captured_source_sequence`, `captured_source_digest` and `accounts[]`, with each row carrying `account`, `account_revision`, `account_sequence`, `account_digest`, `checkpoint_height`, and `checkpoint_hash`. IDs need not be consecutive. A missing or malformed catalog refuses; it never returns a successful prefix.

The desktop selects a real returned account, with no default-account probe. Changing selection invalidates old replies and clears the old balance, address and operation view. A payment in progress prevents retargeting to another account. Failed discovery disables payment controls and leaves the balance Unknown. A successful empty catalog can expose an explicit account-zero setup action; only the backend's genuine catalog/seed ownership policy may authorize creation. An empty response never authorizes key regeneration or bypasses recovery.

## Calls and responses

All wallet requests below also carry `wallet_binding`.

| Call | Request fields | Principal result fields |
|---|---|---|
| `wallet.orchard.createaccount` / `getnewaddress` | `account` | `address`, `account`, `revision`; creation requires `requires_sync:true` |
| `wallet.orchard.queueshield` | `account`, `request_id`, `expected_revision`, `payments`, `fee_una` | `operation_id`, `account`, `account_revision`, `durable_state`, `proof_queued`, `existing_request`, `archived` |
| `wallet.orchard.queuespend` | Queue-shield fields plus transparent `outputs` | Same queue result |
| `wallet.orchard.finishshield` / `finishspend` | `account`, `request_id` only | `durable_state`, `txid`, `admitted`, `already_in_mempool`, `submission_code`, `submission_message` |
| `wallet.orchard.listreceived` (draft) | `account`, `offset`, `limit`, optional `expected_revision` | Account/checkpoint/source identities, completeness, exact page bounds and receipt rows |
| `wallet.orchard.listoperations` | `account` | Account revision/delivery checkpoint, captured source checkpoint, `operations[]` |
| `wallet.orchard.getbalance` | `account` | `confirmed_una`, `reserved_confirmed_una`, `unreserved_confirmed_una`, account/revision/checkpoint and `account_caught_up_to_captured_source` |

Balances are authenticated checkpoint observations, not lasting spendability certificates. A lagging checkpoint is displayed as synchronizing. Missing, malformed, wrong-account or stale-revision results display Unknown. The UI rejects invalid integers and inconsistent reserved/unreserved sums.

`orchard.getactivationstatus {}` returns network, selected tip/hash, next height, configured height/branch (or null when unscheduled), state, active-at-tip/next-block flags, wallet backend availability and storage mode. The UI validates these fields against the selected connection's network; unknown or inactive rules disable payment effects. The source command does not schedule activation. No countdown or production height is embedded in this UI.

## Payment identity and resumption

A new payment owns one nonzero 32-byte request ID and its original recipients/amounts/fee. A queue retry retains this identity. Once the node owns the stored request, completion sends only account and request ID; it does not choose new inputs or create another payment. Structured `proof_not_ready` with `proof_state` distinguishes queued/running/missing/failed/ready observations. Stale revision and request-ID conflict are distinct errors; the UI does not classify proof status by error-text matching.

`listoperations` rows carry operation ID, reserved/signed state, optional transaction ID and authenticated chain observation. Completion discovery adds `completion_method` only when the authenticated stored request has exactly one committed shield/spend kind. The UI allowlists the two finish methods. Legacy rows without this field stay visible and cannot be guessed into a resumable payment.

After reopening, discovery alone never sends a payment. The user selects a stored operation and confirms **Resume saved payment**. Selection, binding, generation, revision and operation details are checked again after the dialog. This flow holds no recipient or fee data and cannot queue a new payment. Recipients are not persisted by the UI. Unknown pending operations block starting another payment in the selected account.

Queued, proving and proof-ready states are distinct from submission. Admission or already-in-mempool means Submitted, not Confirmed. Confirmation/conflict comes from the account's authenticated block observation; reorgs can change that observation. An in-flight request may finish for the original wallet after a UI switch; the app warns and requires that wallet/node to be checked.

## Remaining work

- Compile and execute the incoming-history parser/widget cases and retain the qualified account-selector/resumption cases in fresh Qt ON/OFF builds and mapped project sanitizer checks.
- Qualify actual desktop-to-node requests and packaged desktop builds. Passive injected replies do not prove this composition.
- Qualify the drafted authenticated incoming-history backend and desktop view, including spent receipts, revision-bound pages, reopen/reorg and unavailable legacy history. `listoperations` contains initiated operations; current unspent notes are not complete incoming history.
- Complete whole-node restart/reindex/reorg, platform/load and release gates before enabling the feature for release.

Legacy shielded-holder recovery and private Orchard covenants remain outside this UI's scope. No legacy “move everything out” action is added.

## Incoming-history composition draft

The received-history candidate adds `wallet.orchard.listreceived` pages of 100 from the draft backend receipt owner. Account/revision/offset/limit/checkpoint/source/EOF fields and every row are validated before display. External receipts and internal change remain distinct; spent receipts are included without a spendability claim. Next/previous retain expected_revision, while refresh starts at the latest revision. Old transport tags/wallet generations cannot replace a current page. Malformed, stale, wrong-account or incomplete legacy history clears the display; it is never an empty-history claim.

Only one page is displayed; no prefix is labelled the entire history. Amounts format exactly in DIN; txid/recipient/memo hex are shown as plain item text/tooltips. No private history is persisted by the app. This is uncompiled composition work atop the Qt account candidate and unqualified backend received-history draft; it establishes no GUI/backend/HTTP/IPC/release readiness.
