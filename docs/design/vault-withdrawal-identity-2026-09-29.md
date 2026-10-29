# Withdrawal request identity and checked enqueue

The actual withdrawal queue now creates nonzero 128-bit IDs with checked OpenSSL randomness. It no longer uses a process-local clock/counter. Both existing request and state maps refuse a duplicate, including completed or failed requests; no retry rewrites a prior identity. Test-injected zero identities refuse. Map insertion rolls back its first insertion if the second cannot complete.

Per-account enqueue bounds use checked subtraction, existing outstanding totals use checked addition, and queue depth refuses an unrepresentable count. These checks preserve the existing accounting and cap policy; they do not introduce spendable reservations or change withdrawal settlement/loss formulas. No signing or external action occurs during enqueue. Existing requests, payloads, states and ledger entries remain unchanged on refusal.

Four common patched-path cases use actual queue/ledger and service classes with a synthetic signing backend: independent default generators, zero/duplicate identities and prior states, throwing generators, and cap arithmetic at the representation boundary. Existing vault suites retain all fixtures and deadlines. This does not establish deterministic uniqueness across all backups, authenticated vault initialization, durable queue recovery, signed-request dispatch integration, or release readiness.

Locally qualified.

## Wallet crypto header portability

Completed request-owner Linux builds exposed that `wallet_crypto.h` used `uint8_t` without including `<cstdint>`. This batch adds its direct standard include. No crypto declarations, algorithms, fixtures, assertions, or deadlines change. Fresh full ON/OFF builds include the actual request-owner/dispatch fixture; actual Linux qualification remains required. The preceding local macOS pass did not establish GCC header self-containment.

## Storage daemon failure propagation

The completed staged-state full workflow showed CheckpointRetentionDaemon failing while its Actions step reported success. The step piped CTest through tee under the default bash -e shell, masking CTest’s status. Use CTest --output-log and keep the same two-test selector, verbosity, sequential execution, and deadlines. A bounded shell check of the patched step with a stand-in CTest verifies that success returns zero and failure propagates its nonzero status. This is workflow status propagation only; it does not diagnose or repair the checkpoint daemon failure.
