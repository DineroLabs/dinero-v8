# Canonical source for pool found-block reports

Locally qualified with fresh full dinerod, replay, lifecycle and wallet RPC targets ON/OFF;38selectedCTests passed each. Six new actual RPC/manager cases ON and two source-independent cases OFF. Two complete linked project-C++ graphs freshly ASan/UBSan instrumented:90replay cases across20groups and64wallet cases across10groups. Exact sources/maps/counts privately retained. Selected block/Merkle/coinbase reward binding before pool accounting; worker metadata synthetic. Not Stratum authorization, pool coinbase ownership, payment-time eligibility or release readiness. External/Rust/C/PQClean uninstrumented; macOS LSan off; actual DaemonApp/OFF outside instrumented graphs. No unsafe originals or synchronization-removal controls. Unsigned share difficulty/height persist as SQLite int64; checked readers preserve exact difficulty and fraction through reopen and refuse malformed negative rows. Initial ON full targets passed but37/38CTests passed; unchanged boundary assertion exposed signed narrowing and is retained.

The actual `pool.submitshare` handler now captures its configured pool manager and, for a valid non-stale found-block report, pins the actual chainstate service and acquires selected-chain ownership before pool accounting. Height and hash must match its coherent live, durable, validated and coin tip. The recorded block header and complete transaction-ID Merkle root must agree, with mutated trees refused. The first indexed transaction must be the exact historical-representation coinbase. Every output must be transparent and the bounded total must equal the reported reward. Missing body/index, stopped or inconsistent source, disconnection, or a mismatched reward refuses before the pool transaction.

Selected-chain ownership remains held through the existing DB-only share, worker, round, block and deduplication transaction. The same pool manager receives the canonical lowercase block hash, avoiding a separate block identity based on hexadecimal casing. No wallet signing, submission or external callback runs inside this owner. Ordinary shares and stale/invalid classifications retain their existing path. The actual manager now refuses difficulty outside the positive finite range below2^32 before its legacy uint32 conversion, and refuses zero or over-supply found-block rewards. Fractional representable difficulty retains its existing truncation into the legacy field and exact real-valued storage.

## Limits

This binds a reported block and reward to the selected chain at accounting time. It does not authenticate the worker, Stratum job or proof of work of a share, prove the pool owns its coinbase, independently revalidate historical fees, or certify future payout eligibility. An existing transaction index is required; missing data is not reconstructed. The pool reward/subsidy accounting convention remains unchanged. Pool tables and prior blocks remain checked metadata rather than an authenticated complete catalog. Canonical maturity at payment time, source-block orphan debt, uncertain-attempt recovery, deletion/backup completeness, notification-provider installation and whole-node/release readiness remain open. Mainnet activation remains unset.

## Planned validation

Six actual component cases use the real RPC registry and manager/SQLite: absent selected source; bounded fractional difficulty and deduplication; historical and mixed-block exact coinbase reward; wrong hash/height/reward with unchanged accounting; disconnection/unavailable body and recovery; required block write/COMMIT refusal with exact reopen deduplication. Historical archive metadata and its coinbase transaction-index entry are explicitly installed by the established chain fixture. Pool worker/address metadata is synthetic. These are component calls, not authenticated network transport or independent Stratum authorization. No original-source failure or unsafe control is claimed.

Fresh full dinerod, replay, lifecycle and wallet RPC targets ON/OFF are required with38selectedCTests each. The six new cases compile ON; two source-independent cases compile OFF. Both complete linked project-C++ graphs must be freshly ASan/UBSan instrumented:90replay cases across20groups and64wallet cases across10groups (154total). Exact linked sources are determined by fresh maps. External/Rust/C/PQClean are uninstrumented; macOS LSan is off; actual DaemonApp/OFF are outside these instrumented graphs. Preparation is not qualification or release readiness.


## Unsigned share persistence

The initial normal ON qualification exposed unsigned share difficulty narrowing in
SQLite persistence: `4294967295.5` was accepted but its legacy integer field was
stored as `-1`. Share difficulty and block height now use SQLite's signed 64-bit
binding, which represents the complete uint32 range. The worker history reader
uses the existing checked share decoder, including terminal row completion and
an unsigned limit bound through a 64-bit binding. Existing malformed negative
rows are refused; they are not rewritten or reinterpreted as valid difficulties.
The existing boundary assertion is retained, with exact fractional value checks
through both readers and reopen. Initial failed qualification is retained privately.
