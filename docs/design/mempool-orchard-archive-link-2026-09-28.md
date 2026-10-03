# Optional Orchard archive closure for mempool fixtures

The retained-file, typed-package and pending-address fixtures link the real core,
wallet and storage code. With Orchard enabled, core also reaches runtime block
reading and wallet delivery. GNU static archive scanning must include that whole
project archive closure in the existing rescan group. The earlier reader/backend
addition left these runtime libraries outside the group, allowing wallet objects
to introduce references after core/storage had already been scanned.

The three Linux link groups now include the optional runtime, delivery, account,
operation, block and state archives. Generation-time target presence preserves
backend-OFF builds and CMake 3.20 support. Existing non-Linux library lists,
production code, fixtures, assertions, deadlines and workflow execution gates
are unchanged. No substitute storage, stubs or whole-archive extraction is added.

Fresh declared ON/OFF builds and actual fixture execution are required. A Mac
build cannot establish GNU linker behavior; exact Linux qualification remains
required. This build repair does not qualify Orchard admission or release.
