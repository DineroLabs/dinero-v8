# Typed Orchard service disconnect

The actual `ChainstateService::DisconnectTip` now selects the Orchard route
before historical `Block` decoding. The route requires the exact active tip,
its checked parent index/header/work, restored stateful memory and indexed full
body bytes. It uses `PreparedOrchardChainstateWrite::DisconnectIndexed` while
holding the service activation mutex. The owned write checks full reverse state,
conventional undo and exact flatfile locators, commits the combined rollback
synchronously, and publishes prepared coin/forest memory. The service then
invalidates its non-consensus position cache and publishes the parent tip.
Canonical proof lookup continues to use forest leaves rather than that cache.

A mixed block cannot use the historical wallet/mempool notification signatures.
The service therefore requires a `RuntimeBlockNotifications` provider before
preparing a write. The provider prepares a complete typed event before durability
and must retain anything needed beyond the call. Null/refused preparation means
no commit and no event. Publication is `noexcept` and happens after both durable
state and the service tip agree. It must handle every required wallet, mempool,
proof-cache, relay, long-poll and oracle effect, or arrange durable/recoverable
handoff before acknowledging completion. Preparation must neither publish an
event nor mutate canonical state/re-enter chainstate writers. Post-durable
synchronization or notification failure must terminate rather than return with
partly published state.

**No production notification provider is installed yet.** The actual route
therefore refuses Orchard rollback in a running daemon until its consumers are
implemented and qualified. This is an intentional readiness condition, not a
wallet notification silently omitted after a successful commit. Historical
notifications are unchanged. Mainnet activation remains unset. ConnectTip,
complete startup/replay/reindex, stateless mode, pruned state and ordinary CT
compatibility remain separate unfinished integration work.

The independent `OrchardServiceDisconnect` test invokes the actual service on
honest generated stores, without a historical block validator or fake Block
conversion. A test notification provider inspects exact typed bytes before the
write and observes durable tip, live coin/forest state, active pointer and cleared
position cache at publication. The fixture exercises both a descendant rollback
and the activation-boundary rollback, with and without checkpoint restoration.
It checks absent/refused notifications, inconsistent parent, unsupported CSN,
unchanged logical storage on refusal, complete memory-to-disk agreement after
success, retirement undo at the boundary and rejection of repeated stale undo.
The old service fails the new success assertion. This is real-service fixture
coverage, not a running-node lifecycle or release binary qualification.
