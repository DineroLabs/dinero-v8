# Orchard spend request RPC

Locally qualified with fresh full declared ON/OFF targets;79 ON and74 OFF selected CTests passed plus enabled ON OrchardRuntimeReader. All linked project C++ freshly ASan/UBSan:187 replay,122 wallet,35 state-machine cases and five separate ordinary DaemonApp modes. Seven new ON/two OFF RPC component cases executed. Exact source/input/maps and private evidence independently verified. External/Rust/C/PQClean uninstrumented; macLSanoff; normal9regression binaries/reader/OFF outside sanitizer graphs. Actual registry/component requests; HTTP/auth transport and public completion/admission/relay/shielding host remain open. No release readiness; mainnet unset. Full-width Base58 payload decoding retains leading zeros; healthy 21/33/78-byte payloads and two fixed vectors executed in the backend-independent case. Completed-observation retry is tested before explicit archive staging, followed by archived retry. The predecessor ON run failed two RPC cases and remains preserved privately; no original-source RED or omission-control claim.

`wallet.orchard.queuespend` is the actual registered JSON component boundary to the authenticated request owner and service proof executor. It requires six named fields: account, nonzero32-byte hex request_id, positive expected_revision, ordered payments and outputs arrays, and explicit integer fee_una. Amounts are positive integers in atomic units; no floating point/string/bool coercion. Orchard payments accept address, amount_una and optional memo_hex up to512bytes (zero-padded). Transparent outputs accept address and amount_una. Unknown/missing members, embeddedNUL, malformed/oddhex, bounds and empty recipients refuse.

Capture the selected service source under service lifetimes before wallet SQLite/key ownership. The actual request owner rechecks session and authenticates every declared/reached current/archive owner. Decode Orchard addresses against the captured domain network. Transparent decoding checks the same explicit HRP and canonical script/version: v0 P2WPKH/P2WSH, v1 Taproot, v3 P2MR; unknown versions refuse. Base58Check validates exact21byte payload, canonical encoding and Dinero-network P2PKH/P2SH version. Testnet/regtest share legacy version bytes; that historical encoding cannot distinguish them. No hash-of-address fallback, HD paths, key recreation or relabeling. Aggregate amount/fee range checked before queue ownership.

Durable request commitment binds actual wallet/domain/account, ordered recipient/memo/output bytes and explicit fee. Retry preserves original reservation/current or archive, returns original committed state and never enqueues another plan. New revision is a precondition only for new work. Result includes operation_id, account/account_revision, proof_queued (THIS call publication only), existing_request, archived and durable_state reserved/signed. Allocation/response failure after commit is retried using the same exact request. No signed bytes or proof are returned.

Scope: pure Orchard inputs, real RPC registry/component and actual wallet/service/SQLite/prover in planned tests. Queuing alone does not finish/send a transaction. Public proof collection/authorization/Ready/admission/relay host and transparent-input shielding remain required. HTTP/auth/JSON transport, restart reproving, cancellation/exposure policy and release qualification are separate. Explicit fee is not a decision for the unresolved vault fee payer. No production activation or default mainnet schedule changed.

Tests executed: two parser/registry/backend cases ON/OFF and five actual funded-wallet cases ON, including real shielded transfer/unshield/mixed destinations through existing proof-finalization/mining components, exact ordered script/request retry/reopen/archive, strict malformed/network/money/owner refusal, SQL write/COMMIT and missingcatalog-owner preservation. Seven ON/two OFF cases executed; no original-source RED or omission-control claim. Existing cases/guards/deadlines unchanged; mandatory180sec independent CI lane retains inventory and exact execution markers. All prior qualification required.


## Repair preparation after the first full ON execution

The original OrchardSpendRpc run executed seven cases: five passed and two
failed. The mined-request case expected archival immediately after observing
completion; the existing owner keeps it current until StageCompleted. The
prepared fixture now verifies that current retry is read-only, calls the actual
ArchiveCompleted helper, and preserves the original archived-retry assertions.

The valid transparent-destination case exposed Address::base58Decode's fixed
64-bit conversion, which cannot preserve a full versioned address payload plus
checksum. The prepared repair uses byte-vector radix conversion and preserves
leading zero bytes. It retains checksum and network rejection. Healthy round-trip assertions in the existing backend-independent
case cover versioned payload and extended-key widths, leading zeros
and two fixed valid addresses. It does not execute an unsafe original or a race,
churn or synchronization-removal control.

This eight-file repair preparation is UNCOMPILED, UNEXECUTED and NOT QUEUED.
The failed build is read-only once its remaining tests finish. A new build name,
fresh full ON/OFF targets and all actually linked project sanitizer graphs are
required. Copied qualification helpers/plans remain inherited historical inputs
until explicitly revised for this repair; they are not completion evidence.
