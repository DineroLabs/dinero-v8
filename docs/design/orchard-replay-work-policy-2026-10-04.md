# Orchard replay work budgets

Selected-parent preparation previously imposed an absolute limit of 100,000 blocks, even when a complete longer history fit its serialized-data budget. The independent-parent fallback used the same limit. Both now derive their record limit from the unchanged 256 MiB budget divided by the fixed 128-byte historical header size. Every serialized body contains that header, so a history exceeding this record limit cannot fit the byte budget. Complete bodies are still charged and validated. The resulting limit is 2,097,152 records; it does not promise capacity for that many full blocks or select an activation height.

Prepared branch replay separately limits its retained post-activation suffix to 100,000 blocks. Both branch constructors and the service use the same predicate before retaining branch headers. Empty or reversed ranges and UINT32_MAX refuse. Parent and branch material remain charged together against 256 MiB. Profile and domain checks, proofs, source rechecks, serialization, and synchronization are unchanged. The separate wallet-rescan policy is unchanged.

## Qualification scope

These are operational work bounds, not consensus limits or resident-memory guarantees. Selected state, replay state, forests, indexes, and retained branch transitions have additional memory costs. The changes require fresh ON/OFF builds, actual service execution at the larger parent height, signed branch execution, relevant regressions, and fresh linked instrumentation. Smaller prior checks and a history that fits the byte budget do not establish general capacity, production timing, whole-node recovery, platform qualification, or release readiness. Mainnet activation remains unset.
