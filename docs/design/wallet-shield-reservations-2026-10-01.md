# Shield requests and transparent wallet reconciliation

Prepared source only: 24 changed files, 40 written backend-ON cases and two backend-OFF cases. This packet has not been compiled, executed or qualified. The planned checks below are requirements, not results.

## Durable request and reservation

The actual account owner authenticates the current catalog, reached archives and retained parents in one checked FULL wallet transaction. It requires caught-up accounts, available keys, exact wallet input amounts/scripts and nonoverlapping ordinary, Orchard and manual reservations. Inputs are unique and final; change requires an existing signing key. The explicit deposit, change and fee must balance before a randomized plan is created. The account snapshot commits before an unpublished proof ticket leaves the owner; publication happens after selected ownership is released.

DNORSH01 binds the ordered inputs, sequences, amounts and scripts to the wallet identity, network, genesis, account, branch, activation, request ID, recipients, memos, change and fee. DNOROP03 stores complete shield details only when present; predecessor formats remain unchanged otherwise. Checked decoding validates monetary conservation, canonical recipients and exact EOF. Current, reached archived and restored parent operations reauthenticate the request binding. Existing requests are resolved before new coin selection: exact retries return retained state, changed intent refuses, and missing jobs never authorize regeneration or reservation release.

## Normal RPC, selection and completion

`wallet.orchard.queueshield` accepts an account, request ID, expected revision, ordered Orchard recipients/memos and an explicit fee. `finishshield` accepts the same intent and restores its retained inputs and change. Both use the selected wallet, source, backend and safe-mode owners. Unknown completion requests cannot select coins. Archived queue retries return retained status; archived completion refuses resubmission.

Candidate capture validates every selected SQL row and terminal DONE under the lease, seed and transaction owners. It excludes manual locks, ordinary and Orchard reservations, unsupported scripts and watch-only keys. Selection uses actual selected coins, the consensus maturity floor, largest-first ordering and a maximum of 1,024 inputs. The 65,536-row and 16 MiB script limits are operational refusals, not memory or load qualification. A newly issued change address commits separately before reservation; a later failure may leave an unused address and an invalidated old script-domain receipt.

Only the private account owner may sign the retained shield request. It checks the owned proof and intent and uses `OrchardTransparentSigningDigest`. Canonical Taproot and the historical SHA256(internal-x-only-key || 0x00) tweak preserve their exact output-key bindings and parity handling. P2WPKH requires compressed-key hash binding and low-S DER signatures. Secret buffers are scoped and cleansed. Unsupported programs refuse; no invented HD path or replacement key is used.

Proof/source capture runs outside selected and wallet locks. Completion reacquires selected ownership, rechecks the exact head/profile and coins, verifies transparent and Orchard authorizations and selected pool validity, then commits Ready in a checked FULL transaction. Snapshot-write or COMMIT failure preserves the proof and reservation. Executor retirement and returning preallocated signed bytes follow Ready commit. Admission occurs after owners are released. Exact Ready retries reuse retained bytes.

## Authenticated local history

Reserved-to-Ready assigns a local timestamp once and seals it in DNOROP04; older formats remain unchanged when this field is absent. The local `shield` summary records the exact transparent debit including the explicit fee. One recipient supplies the summary address; multiple recipients have an empty summary address, with their complete details retained in the operation. Callers do not supply replacement history fields or fabricated labels.

The history append and Ready replacement share one FULL transaction. Exact retries require matching history and its original timestamp. Missing or altered rows, and old Ready records without this owner, refuse without reconstruction. Current accounts, reached archives and restored parents validate authenticated history. A history append can preserve only an already valid ordinary receipt after an exact database comparison excluding that new history row and receipt fields; it cannot create a baseline or reset an invalid receipt. Pending-payment owners remain unchanged.

## Immutable source coverage

`getRuntimeWalletCoverage` captures actual ordinary/index script coverage from independently validated origin replay through the checked runtime head. It requires the correct wallet and index lifetime owners and an existing nonzero persistent identity. Borrowed SQLite/selected ownership and wrong sessions refuse. Source acquisition and proof verification occur outside selected and wallet locks; exact source head, tip, identity and script domains are rechecked before effects.

The projection retains current coins and transaction activity plus observed creation/spend versions from actual origin and runtime events, including disconnected branches. Disconnects reverse transactions in child-before-parent order, and reconnects rebuild selected facts without mutating prior captures. Additional material is charged to the existing 64 MiB traversal cap. These source facts do not independently certify all historical consensus, header work, undiscovered keys or backup completeness.

## Reconciliation in normal recovery

Normal catalog recovery classifies missing, invalidated or changed-script-domain receipts using checked reads. Malformed metadata, read errors and borrowed transactions refuse. When coverage is required it captures the source outside owners, rechecks the exact head and domains under selected ownership, reconciles the stores, then obtains a fresh source for account replay.

One FULL main-wallet transaction authenticates present HD/imported signing owners, exact scripts, available PQ seed/public/root/address bindings and durable masters, the account catalog and archives, shield history and ordinary/Orchard reservations. The account reader participates in the caller's transaction without beginning or ending it. Watch-only rows remain recognition metadata. The separately captured read-only PQ store is not an atomic cross-database catalog; absent stores or rows cannot certify deletion/backup completeness or authorize generation.

Every existing coin must match a proved observed creation/spend version. Confirmed history must match a proved transaction, height and kind. Every retained ordinary or current Orchard input must match an observed amount and script; an authenticated account snapshot alone does not prove an input existed. Unknown rows, unavailable signing owners and missing originated-send history refuse before index publication. A disconnected but observed input remains reserved.

The index commits first and the ordinary wallet second while owners remain held. Proved former creations absent from the selected branch are removed individually. Retained coins update chain fields while preserving local and auxiliary metadata. Disconnected history is unconfirmed without deleting category, amount, label or time. Receive/mining history may be derived from actual source; originated-send metadata must already exist. Pending bodies are neither discarded nor resubmitted. A second-store failure leaves a genuine index prefix and main-wallet rollback for retry.

Checked DNUI01/DNOW01 receipts bind the actual captured prefix only after validation and store updates. Existing ordinary delivery/adoption bodies remain unchanged. A completed pass is not a lasting all-consumer readiness acknowledgment.

## Qualification requirements and limits

The source contains 19 reservation/detail, six ON/two OFF RPC, three history, five coverage and seven reconciliation cases. Reconciliation includes actual shield recovery, disconnect/reinclusion, exact history preservation, COMMIT refusal/reopen retry, unknown coin/key refusal and retained Orchard inputs outside proved history. Existing assertions and deadlines are retained. No original-source RED or omission-control claim is made for these additions.

Required fresh checks are full declared targets in genuine ON and OFF configurations; 89 ON/77 OFF selected CTests including nine link regressions each; three additional ON reader/account/archive CTests; 409 GTest cases across three freshly instrumented linked-project graphs; five ordinary DaemonApp modes and two custom account/archive graphs. Actual commands, complete linked source, input hashes, maps, case markers, counts and exits must be verified before publishing qualification. External/Rust/C/PQClean dependencies remain uninstrumented; macOS LSan is off. Normal OFF, reader and regression binaries are outside those instrumented graphs.

Dedicated historical/P2WPKH signing, maturity-boundary and broader conflict/load qualification remain open. Present-owner validation does not establish deletion, rollback or complete undiscovered-key/account recovery. Multiprocess and cross-PQ-database atomicity, whole-node power-loss/restart/reindex/reorg, long-history/capacity and platform/release gates require their own evidence. Full tests containing excluded unsafe cases remain unresolved. Mainnet activation is unset; no deployment or release readiness is claimed.
