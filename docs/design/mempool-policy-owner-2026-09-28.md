# Owned mempool policy settings

Scalar size, expiry and minimum-fee settings now share the pool mutex with admission and maintenance. CT configuration reads return a value snapshot. Individual CT field setters mutate only their field under the same lock, and both RPC implementation files call those setters without retaining a mutable reference into the pool. Existing RPC ranges, field values and result formats are preserved.

Three patched-path component fixtures check detached snapshots, two independent field writers with readers, and genuine signed admission while a configuration worker updates bounded settings. Worker guards stop and join on fixture exceptions. No unsafe-original race reproduction or synchronization-removal controls are run. ASan/UBSan scope is separate from TSan; successful threaded cases do not establish all possible schedules. The active ct_fee_rpc adapter is compiled in the full daemon but not executed by these component cases. The legacy src/daemon/rpc_server.cpp adapter is excluded from the declared build graph, so its compatibility edit is not build/runtime qualified.

This is settings ownership, not validation of every configuration range or nonfinite value, repricing of existing cached entries, raw coin-view/factory/logger/scanner lifetime, all-consumer/provider/Orchard/load/release qualification. Mainnet activation remains unset.
