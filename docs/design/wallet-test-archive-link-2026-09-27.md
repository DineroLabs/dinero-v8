# Wallet-dependent test archive links

Linux default, QUIC and core-heavy builds at `660f362b` linked the repaired
header metadata fixture, then failed to link `test_template_maturity` and
`test_wallet_input_coins`. Their transitive wallet archive references chainstate
and core symbols after those archives have already been scanned.

The two targets now group the actual mutually dependent project archives on
Linux, following the existing `nodecore_runtime_driver` arrangement. Raw linker
group flags retain the CMake 3.20 minimum. Other platforms keep their existing
link lists. Production code, fixtures, assertions, deadlines and the real mempool
implementation are unchanged. Existing vault test stubs remain unchanged.

The independent Orchard lane now builds both targets and requires their enabled
CTest registrations plus all twelve existing passing GoogleTest case markers.
It retains inventory and verbose execution logs. The two existing root selectors
still contain 46 registrations. Actual CI executions determine test totals.

Fresh local backend-on/off declared daemon and target builds passed, with
both actual CTests and all twelve cases executed in each configuration. The
backend-off cache and service compile graph were independently checked. The
unchanged two 46-test root registrations were inventoried, not all executed. macOS does not reproduce GNU archive scan order;
local passing links do not establish that the Linux failure is fixed. Fresh Linux
default, QUIC and core-heavy builds must complete on the changed source. This
CMake-only change adds no sanitizer or whole-node qualification.
