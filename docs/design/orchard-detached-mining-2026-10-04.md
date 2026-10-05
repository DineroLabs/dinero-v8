# Detached Orchard mining template validation

The mining reader captures the selected parent and the complete values used by a candidate while holding the selected-chain owner. It releases that owner for transaction authorization and forest-proof work, reacquires it for commitment capture, releases it for final proof completion, and reacquires it for final publication checks. The packet pins its service and storage owners and remains thread-affine.

Publication requires the same selected parent, profile, operator generation, captured coins and absences, transaction locations, median-time values, anchors, nullifiers, commitment membership, retirement state and forest identity. Captured coins are checked through the existing database/live view before invoking the existing journal audit. A stale packet is refused without turning that speculative rebind into a new journal diagnosis; every later journal and selected-state check remains required.

The mining RPC passes its existing root reader to the assembler. A nested reader cannot release an outer owner's lock and refuses template construction. Both normal and exceptional detached completion reacquire the root reader before returning. An unmined template never substitutes for a fully validated block token.

The component cases cover exact template preservation, held-owner refusal of detached phases, changed coins before commitment and after proof completion, retry after restoring the captured state, final coinbase collisions, changed profile, borrowed-root construction, nested refusal and exception reacquisition. The backend-OFF case requires default refusal. These serialized tests do not establish race, crash, power-loss or throughput behavior.

## Remaining scope

Pool transaction selection still performs validation while holding its existing owners. This change does not remove all mining or admission proof work from locks. Full selected-parent history capacity, target-height resource qualification, all platform/dependency qualification and production rollout remain separate requirements. Mainnet activation stays unset. Short regtest and component success are not evidence of production readiness.
