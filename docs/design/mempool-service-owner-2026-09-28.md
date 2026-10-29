# Own mempool service operations through shutdown

Typed ingress/query/template methods and their synchronous compatibility wrappers pin a service operation before using the pool. Long prepared notification jobs acquire a guard that also retains shared service ownership. The actual legacy connect/disconnect notifier now retains that guard through pool/cache preparation, publication and refresh callbacks.

Stop closes independent acquisition and waits for all active operations before clearing/resetting the pool or dependency links. The wait releases the short service mutex and holds no selected-chain or pool lock. Nested same-thread callbacks belonging to an existing operation can finish during drain. Reentrant Stop from that operation refuses before changing lifecycle state. Log failure does not strand shutdown, and pool/callback/dependency destruction occurs outside the service gate. A second Stop waits for the first completion.

Init/Start remain serialized startup operations. Stop must be invoked outside selected-chain ownership under existing daemon shutdown ordering. Legacy escaped mempool references and asynchronously retained raw references remain caller-owned and are not retroactively pinned. Direct independent ChainstateService shutdown, arbitrary lifecycle reinitialization, external raw logger lifetimes and all unrelated dependency races are outside this change. Component fixtures install an explicitly published isolated pool through a narrow friend seam; full daemon fixtures separately exercise actual Init/Start/Stop. No unsafe-original or synchronization-removal execution is needed.

This establishes scoped operation ownership, not durable mempool persistence, Orchard admission/readmission/provider completion or release readiness. Mainnet remains unset.
