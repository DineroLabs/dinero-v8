# Vault selected source snapshot

Locally qualified. Prepared against the future staged-state revision; not applied or executed.

Production daemon wiring captures the exact requested selected tip and every non-reverted deposit in one source read under the existing chainstate lifetime and selected locks. Each result binds its outpoint, height and transparent amount to the actual historical or typed body. A height above the observed tip has an explicit absent result; unavailable storage and incomplete reads throw. No pruning or missing-body absence is inferred.

VaultService captures its deposit queries and monotonic in-memory revision under its mutex, releases the mutex before source acquisition, then requires the same revision and complete ordered result bindings before staging all reconciliation and lifecycle work. Every published transition advances the revision; the existing externally-effectful withdrawal driver invalidates earlier captures before entering its work, including on failure. Publication remains nonthrowing. Exact-tip mismatch and intervening service changes require a fresh call. Injected legacy per-deposit readers retain their narrower scope.

This is an as-of selected observation with atomic in-memory publication. It is not durable vault recovery, continued chain readiness, whole-wallet ownership, notification completion, all-deposit resident/load qualification, or independent historical validation. Existing loss-accounting, reactivation, withdrawal inclusion ownership and signing/broadcast durability remain open. No provider or activation is introduced. No unsafe-original or synchronization-removal controls.

Five patched-path cases are required: complete single capture, controlled intervening publication/refusal/retry, malformed or unavailable capture preservation, actual historical plus typed canonical bodies/disconnect/reconnect/storage refusal, and actual runtime integration with stale-tip refusal. Three cases are shared with backend OFF; two require the real backend. The separate actual VaultService and VaultStateMachines suites and all prior fixtures/deadlines remain required.
