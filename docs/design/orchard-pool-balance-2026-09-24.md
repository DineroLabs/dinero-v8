# Orchard transparent-value pool guard

This is a staged integration rule. Production admission and block connection
still reject the new format; this change protects no running mainnet pool yet.
No activation height or historical validation rule changes here.

## Arithmetic independent of the proof

For each Orchard transaction, derive the public value flow from its owned,
authenticated transparent coin snapshot and exact outputs and explicit fee:

    next_pool = previous_pool + transparent_inputs - transparent_outputs - fee

`GetOrchardValueFlow` takes the sealed transparent-authorization result. It
does not read an Orchard proof, note amount or claimed bundle balance.
`ApplyOrchardValueFlows` uses checked unsigned arithmetic and bounds each
amount, outputs plus fee, and every resulting pool value by `MAX_MONEY`.
Fees funded from the pool are withdrawals too. An empty list preserves the
balance. An absent initial Orchard state means zero; legacy value is not
carried into this counter.

The draft rule applies transactions in canonical block order and requires the
pool to remain in range after **each** transaction. A later deposit cannot
rescue an earlier withdrawal. This stronger intermediate-state rule needs
protocol review before activation; it must not be described as identical to
ZIP 209's final-block pool-balance rule.

## Storage and caller obligations

`stageOrchardConnect` requires the flow list and independently recomputes the
counter. It rejects a proposed stored balance that differs from that result,
before changing the caller's batch. The existing full-state undo restores the
old counter along with Orchard state and nullifiers. The caller must stage
transparent coins, chain tip and the rest of chainstate in the same write batch.

The low-level storage API takes values, not authorization certificates. Its
future runtime caller must supply **every** Orchard transaction exactly once,
in block order, using current authenticated coins under the writer lock. A
partial flow list or fabricated inputs cannot be made safe by storage alone.
This guard does not replace proof verification, transparent conservation,
anchor eligibility, nullifier checks, or correct coinbase fee accounting.

The guard prevents withdrawals beyond the pool's recorded resources. It does
not protect depositors from a broken proof system: their current funds and
later deposits remain exposed. A current pool balance is not a lifetime bound
on theft. Dinero's signing, parsing and state integration remain security
critical even while upstream supplies the cryptographic implementation.

## Executed checks

- Real synthetic shield and spend authorization fixtures produce independently
  expected flows. Rejecting an altered proof does not change the public flow.
- 4,096 boundary combinations compare the counter with a separate signed
  128-bit arithmetic oracle, including machine overflow and money limits.
- Fee-only withdrawal, exact drain, ordered underflow, cumulative withdrawals,
  and maximum-value turnover are covered.
- Real temporary ChainDB stores reject inconsistent next balances and initial
  carry-forward without funding; drain, reopen and disconnect restore the
  previous counter. Rejected staging preserves the existing caller batch.
- Removing only the storage balance check in a temporary source copy makes the
  regression fail. Removing nullifier undo independently also fails.
- Root Orchard CTest: eight tests passed. Standalone component: six passed.
  C++ storage/ChainDB and authorization host sources pass ASan/UBSan; linked
  dependencies are not all instrumented, and macOS leak detection is disabled.

The sanitizer run also identified a fixture error: `setTip` records the current
write time. Undo checks now require that timestamp to be within the restore
operation and require all remaining database bytes to match exactly. Abandoned
staging and failed writes still require byte-exact equality including time.

These are component and storage checks, not full-daemon crash, replay, reindex,
reorg, wallet or release qualification.
