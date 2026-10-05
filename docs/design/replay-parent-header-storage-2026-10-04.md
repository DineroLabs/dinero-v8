# Parent replay header storage

## Problem and change

`AssumeUtxoReplayEngine` independently validates the selected historical prefix before that prefix can support activation. Its former `HeaderChainSelector` retained a header entry and ancestry pointers for every replayed block, even though this engine only accepts one ascending sequence.

The candidate replaces that private header graph with `ReplayHeaderHistory`, backed by the existing authenticated temporary SQLite replay spool. Each height identifies an exact 128-byte header. The engine retains its current height and hash; median-time calculations read at most eleven timestamps into a bounded array. Old timestamps and timing anchors remain available from the spool.

This change addresses the private header graph. The replay UTXO set, forest, body capture, selected-state comparisons, and other history-dependent allocations still require capacity qualification.

## Validation rules

Both the ordinary header selector and the replay history call the extracted `ValidateHistoricalHeader` rule. The ordinary selector keeps its existing locking and ancestry ownership. It supplies values obtained from that ancestry; private replay supplies values from authenticated spool records.

Historical version, timestamp, proof-of-work, and ASERT rules are preserved, including the existing treatment of uncomputable ASERT context outside the stricter isolated qualification profile. The candidate retains the two existing timestamp semantics:

- Header median-time-past narrows each timestamp to 32 bits before sorting.
- Contextual transaction locks use the original 64-bit timestamps.

The test reference contains the original validation function from before extraction. Its comparisons cover real proof of work and a local 120-to-60-second timing boundary. These tests do not select a mainnet activation height.

## State publication and storage failure

Header validation precedes stateful body validation. The engine appends the authenticated header only after the body succeeds, commits that append, and then publishes its new replay position. No replay-spool transaction or mutex spans script or proof validation.

A normal invalid body leaves the header position unchanged, allowing a corrected body to be retried. A storage failure after body effects retires the private replay engine: subsequent state access and advancement refuse. This prevents partially updated private state from being consumed as a validated prefix. An undo-tail publication failure also retires the engine.

The spool is disposable private replay material, not a persistent chain-validity certificate. Its authentication cannot replace independent block validation or the final comparison against selected state.

## Qualification and limits

Qualification must bind exact source, all actually linked project C++ files, executed case names, and fresh ON/OFF normal builds. Serialized validation-omission controls must demonstrate that the assertions detect missing header checks, ignored commit failures, and narrowed transaction-lock timestamps. Header variants require rebuilding every dependent translation unit. No synchronization-removal control is part of this work.

The 20,000-header pager case covers header storage and old time-window reads. It does not execute 20,000 full block bodies, measure whole-process resident memory, establish full-history performance, or certify release capacity. The existing source limits remain unchanged. Mainnet activation remains unset.
