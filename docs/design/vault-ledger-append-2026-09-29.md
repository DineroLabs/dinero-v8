# Checked in-memory vault ledger append

Ledger append prepares account and outstanding-credit maps and counters before publishing an entry. It copies derived state without copying the entry history. A successful vector append is followed only by statically checked nonthrowing map swaps and integer assignments. Validation, account arithmetic, and allocation failures leave the prior public ledger state intact. Sequence zero and gaps remain valid. An entry at UINT64_MAX refuses because no following sequence can be represented; UINT64_MAX-1 remains valid.

Per-user/global cap checks use subtraction before addition. Account additions check pending, confirmed, combined pending-plus-confirmed, locked and operator-loss capacity. The ledger checks total operator-loss growth before publication. INT64_MIN policy debits use a defined unsigned magnitude. Existing valid lifecycle, settlement, saturation and loss formulas are retained; this is not a correction or certification of their broader economic semantics. Policy changes still have their existing narrower treatment in global loss accounting.

Four patched-path cases exercise actual Ledger append/replay and account numeric boundaries, exact complete-state preservation after refusal, valid zero/gapped/max-minus-one sequences and successful retry. Existing ledger/store/state-machine fixtures remain unchanged. No unsafe-original control, injected allocation failure, power-loss or durability claim. Derived-state copying is not load-qualified.

This is an in-memory prerequisite. Full vault persistence, deposit/request lineage, withdrawal wallet linkage, cross-store recovery, loss-accounting invariants and production notifications remain open. No mainnet activation or release readiness. Locally qualified.
