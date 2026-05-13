// Qt firewall compile-fail probe (Phase 2 Step 5 of monorepo consolidation).
//
// This source file MUST fail to compile when built with dinero-qt's
// include directories. The CMake target `qt_firewall_violation_probe`
// builds it via `cmake --build --target qt_firewall_violation_probe`;
// the CTest entry `QtFirewallProbe` is marked WILL_FAIL TRUE, so a
// compile failure is a PASS for the test — the firewall holds.
//
// Qt is an RPC client. It links Qt6::Widgets, Qt6::Network, and
// dinero-solo-miner, and talks to dinerod over JSON-RPC. It MUST NOT
// reach consensus headers — neither directly nor by transitively
// inheriting include paths from a library that does. If a future
// change widens dinero-qt's include directories to expose
// `consensus/...`, this probe will compile cleanly, CTest will report
// a failure, and the breach surfaces at CI time instead of in
// production six months later.
//
// Two-way verification procedure:
//
//   Forward (default state, firewall holds)
//     $ ctest -R QtFirewallProbe
//     The compile fails (consensus/chain_identity.h is unreachable
//     from Qt's include paths), the test passes via WILL_FAIL.
//
//   Reverse (deliberate breach)
//     Temporarily edit qt/CMakeLists.txt to add
//       target_include_directories(dinero-qt PRIVATE ${CMAKE_SOURCE_DIR}/include)
//     reconfigure, re-run ctest. The compile now succeeds; the test
//     FAILS, proving the probe catches real breaches. Revert before
//     committing.
//
// The #include below is intentional. Do not "fix" it.

#include "consensus/chain_identity.h"
