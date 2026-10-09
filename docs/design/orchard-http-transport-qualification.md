# Isolated Orchard HTTP desktop qualification (draft)

This opt-in test connects the production RpcClient and OrchardWidget to the production
HttpRpcServer, RpcAuth and Orchard handlers using existing real regtest account fixtures.
It adds no production API and changes no activation height or UI default.

**Execution is on hold under the current no-IPC instruction.** Compiling this draft
is permitted; compilation is not a transport pass. Do not launch it, even for listing.
No public/default CTest or CI registration is added. No existing test body is modified.

Proposed future run, only after explicit authorization:

- Fresh private mode-0700 `/tmp/orchard-http-isolated-*` home, isolated XDG directories,
  explicit marker, offscreen Qt, no DINERO_RPC_URL. Environment set before process start.
- Exact `--gtest_filter=OrchardHttpTransport.*`, plus the explicit opt-in environment gate.
- Server binds only 127.0.0.1:39713 before normal RpcClient construction. A collision
  fails immediately; it never probes an existing listener. Fresh cookie authentication,
  no development bypass, no proxy/failover or production configuration.
- Each request has a 15-second reply deadline; handlers drain before fixture changes.
  The future owner must also impose a whole-process deadline and retain exit/results.
- Actual widget catalog/balance/history, stale wallet binding and detailed error routing;
  Qt-built transfer/unshield/shield requests over HTTP, stored-ID completion, exact retry,
  actual fixture admission, confirmations and transfer/unshield reorg observations.
- Server shutdown precedes backend destruction. All chain/wallet data are temporary.

Limits: full payment clicks in the widget, installed application startup, real network
propagation/PoW, crash/power-loss, cross-platform packaging and production activation
remain separate gates. Existing fixtures can use controlled proof executors; do not
claim newly generated production proofs solely from this transport test. No test has
been executed for this draft.

## Compile review revision 2

Revision 1 compiled its translation units but did not link: its new target omitted
`paycollectpolicy.cpp` and the existing daemon legacy-global support file. The exact
failed build is privately preserved. Revision 2 adds those existing link inputs.
It also confines the server's discovery-file global to the temporary datadir until
after handlers stop, and requires the exact displayed balance, incoming-note label,
activation text and enabled receive control. These are unexecuted assertions, not
new qualification claims. No existing test body or production source was altered.

## Test application lifetime

The first authorized four-case HTTP run completed all four case assertions, then exited with SIGSEGV in QGuiApplication destruction during global teardown. That run is a failure, not transport qualification. The test adapter now gives QApplication an explicit scope around RUN_ALL_TESTS for the isolated HTTP filter. It is destroyed after every fixture and handler and before global teardown. The original test assertions, handler drain, server/client order, isolation checks and 180-second process cap remain unchanged. This repair requires a fresh build and successful process exit; it does not change the production application lifecycle.
