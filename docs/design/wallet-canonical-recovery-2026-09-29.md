# Canonical wallet recovery at startup and unlock

Locally qualified.

WalletService routes its active wallet through the existing checked canonical recovery owner before startup's historical height handling or unlock's snapshot path. The service retains the exact manager and the chainstate-owned index throughout the synchronous operation. The chainstate entry checks the real index lifetime gate, rejects caller-held selected locks and nested wallet database leases, captures the current wallet session under a short lease, then releases it before selected source capture. Proof verification and wallet replay happen outside selected-chain ownership.

A valid unset profile, or a checked preactivation source with no retained log, returns NotRequired. A configured source with a retained canonical log uses ResumeEnrolledWalletStores, including after a disconnect below activation. Missing canonical records at an active boundary, unavailable backends, changed bindings, locked keys, missing transparent baselines or absent/unrestorable accounts defer recovery. Startup suppresses legacy height rewriting and historical catch-up when canonical recovery applies or is deferred. The existing wallet.unlock call retains its unlock-success result and reports a recovery warning when recovery remains deferred.

The recovery operation never creates accounts, adopts a baseline, regenerates keys, repairs paths, resets a cursor, fabricates history or turns an integer height into readiness. Its success reports application through a captured canonical prefix for the active wallet's enrolled transparent stores and all present authenticated accounts. Source growth needs another pass. Preexisting absent/deleted accounts, complete key membership, backup rollback, pending-operation completeness, other wallets and other configured consumers remain separate obligations. Startup service availability and successful unlocking are not all-consumer readiness.

## Qualification plan

Six patched-path cases are registered in WalletCanonicalRecovery, with the two owner/profile cases enabled in backend-OFF builds. The positive fixture calls real ChainstateService::Init and WalletService::Init. A narrow fixture adapter populates the proven parent in the already initialized consensus object; it does not replace the index, fake an admission flag or claim full chainstate Start. Existing independently replayed regtest history, proof construction, canonical mining and retained outbox owners supply real events. Account setup uses the real encrypted snapshot store and exact seed-derived wallet identity. Transparent adoption uses the existing explicit origin capture and adoption operations. No arbitrary receipt/cursor setter is used.

The positive cases cover a real 500000-una shielded note, initial account replay, encrypted reopen and actual wallet.unlock recovery, actual WalletService::Start catching up a lagging prefix, missing-baseline refusal, and a required snapshot-update refusal followed by retry. Test and production code were prepared together; no original-red, unsafe-original, race, deadlock or synchronization-removal control is claimed. Required normal ON/OFF daemon/components and all linked project C++ sanitizer coverage must pass before publication.

No production notification provider, network activation, new process crash/power-loss, general-history capacity, load, whole-node or release qualification is established by this batch.

The encrypted recovery fixture also requires configured preactivation without retained events to select historical behavior, then performs a real typed boundary disconnect and requires canonical replay below activation to remove the previously confirmed note. This is sequential patched-path coverage, not a race or unsafe-original control.

The actual wallet.unlock handler retains a WalletUse through both unlock and recovery so its lifetime does not depend on the calling transport. This owner does not hold a database lease across source acquisition or certify a stable selected-wallet session for other handlers.

The unlock handler captures the actual selected session while holding a short database lease over unlock, releases it, and passes that session into recovery. A changed session refuses before selected source acquisition or historical fallback. The existing common fixture verifies stale-session refusal after an ordinary reopen. This does not hold a database lease during replay or certify lasting readiness.

The project sanitizer plan executes the prior 99 cases with their existing 300-second bound and the six new cases separately with the declared 120-second bound. Both invocations use the same freshly instrumented binary and are required; this adds no exclusion or deadline increase for existing tests.

## HTTP predecessor CI target count

The HTTP framing fixture added a twenty-first real mempool runtime consumer. The Orchard workflow still required twenty and stopped before building or executing that lane. This batch updates the exact count to twenty-one while preserving the existing enabled-test, compiled-case ownership, execution and result checks. Both prior and new fixtures remain required. The repaired workflow requires fresh Linux qualification.
