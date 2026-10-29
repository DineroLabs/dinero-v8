# Ordinary wallet facts at the checked source origin

`ChainstateService::getRuntimeWalletOrigin` captures the selected wallet session, persistent delivery identity and existing ordinary script domain under the wallet lease, then releases it. The domain reuses the existing address/watch-script union and path binding used by `DNOW01`; it is not a new identity or receipt.

Under the activation lock it obtains event 1 from the actual checked outbox reader. It requires that event to be an activation-boundary connect and follows its actual parent hashes and stored heights back to the selected genesis. Only ancestry hashes are retained. Each actual archival body is then acquired under the chain lock and independently replayed outside both chain and wallet ownership. The existing genesis-bound replay engine checks header acceptance, ordered input state, body identity, transaction authorization and accumulator commitments under its supported historical rules.

After each successful validation, the owner projects facts for the captured ordinary scripts:

- Every owned transparent creation, including creations spent later, retains its exact output, creation height/time and coinbase flag.
- Spending an owned outpoint attaches the actual spending transaction ID, height and source time to that coin.
- Every relevant transaction is retained exactly, with source height/time and integer owned credits/debits. Outgoing-only spends and self-spends remain visible; change is not invented as a new external receipt.

No full-chain body copy is retained. The source rechecks the captured outbox head and first cursor, then rechecks the wallet session, persistent identity and script domain before returning const ownership. Source/domain changes refuse; previously accumulated in-memory facts are discarded. This is still an as-of result, and any eventual adopter must recheck its domains under its own write ownership.

## Limits and adoption

This component **does not adopt a baseline or install a provider**. It does not write UTXOs, transaction history, account snapshots, index rows or delivery receipts. Initial persistent-identity initialization retains its existing separate commit semantics. The ordinary transaction facts do not prescribe UI categories, fees, labels, local send intentions, pending reservations or orphan-history retention. Those must be reconciled with existing local records before ordered index/ordinary adoption can earn a receipt through the real first source event.

The captured script set is the currently known ordinary domain, not complete key/account discovery or an authenticated account catalog. Index ownership is not included yet. Historical CT outputs and unsupported validator epochs refuse; ordinary CT funds are not retired. The source requires the existing stateful checked outbox service. Pruned/missing ancestry or bodies refuse. A one-million-header ancestry cap and 64 MiB projected-material cap are operational refusals, not resident-memory or general-load qualification. The replay engine still retains its own consensus state and header ancestry. No pages or pending operations are discarded to meet a limit.

## Qualification scope

The original component qualification below is supplemented by
[successful service-origin capture qualification](wallet-origin-capture-qualification-2026-09-26.md).
That later regression qualifies a short regtest success path, while adoption and
production activation-history qualification remain open.

The new projection regression covers spent creations, exact transaction retention, outgoing-only spends, self-spend credits/debits, duplicate-spend refusal and CT refusal. Its unsigned spend fixtures test projection arithmetic, not consensus authorization. Existing owned-replay tests exercise validation separately. Actual wallet-domain tests cover caller-held lease refusal, script changes and same-name reopen/session changes. The actual checked-service fixture has generated prehistory and must refuse origin certification despite its valid local outbox records. A successful complete service-origin capture over independently valid activation history is **not yet qualified**; neither is adoption, startup, reindex, whole-node crash or release readiness.

Fresh declared targets and the full daemon built locally. Three unique CTests passed: `AssumeUtxoReplay` (eight replay cases, the previous selected-source case and two new projection/domain cases), `OrchardServiceDeliverySource` and `OrchardIndexDelivery`. ASan/UBSan covered all 202 linked project C++ translation units in the replay/projection executable; the final test-only correction was rebuilt with the other 201 current-turn objects retained. Projection and wallet-domain methods executed there; the new service-origin method's rejection path was tested in the separate normal service suite. The initial sanitizer finding was a test assertion binding a reference to a packed header field; comparison by value preserves the assertion. External/Rust libraries remained uninstrumented and macOS leak detection was off. This does not close the separate ARM RocksDB gate or qualify successful service-origin capture.

Three copied-source controls omitted the spend record, exact history retention, or wallet-domain recheck. Each failed its intended regression without sanitizer diagnostics; restored source passed. The default-runtime-reader-off service translation unit also compiled. Neither control execution nor an unmodified suite is a substitute for the still-missing successful service-origin and baseline-adoption qualification.
