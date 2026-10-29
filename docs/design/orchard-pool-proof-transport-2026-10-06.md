# Orchard pool input proof transport

The pool's Orchard admission and typed selection previously sent only transaction
bodies to the selected-chain validator. Existing input proof caches were not part
of that validation request. This change carries a borrowed body/proof pair while
the selected-chain owner and pool lock remain held. Cached bytes are untrusted.

The concrete service decodes the entire proof message, requires exact transaction
bytes, authenticates each input's value, script, height, coinbase status and
transparent type against the selected durable/live coin view, then verifies its
inclusion proof against the selected forest stump and leaf count. This independent
metadata check permits old leaves without trusting their uncommitted height and
coinbase fields. Duplicate inputs and immature coinbases refuse. Existing Orchard
signatures, nullifiers, anchor/state, resource and aggregate pending-capacity
checks still run. Pending Orchard entries carry their current proof caches too.
Typed mixed-family selection revalidates supplied caches before returning fees.

Proof-bearing Orchard network admission uses this path. A successful new admission
prepares the exact wire/root/height cache in the entry before pool insertion.
Refreshing an existing exact body repeats validation and swaps prepared cache
buffers without adding another entry or sending a second acceptance notification.
A failure preserves the old entry. A guard without proof support refuses supplied
proof bytes. Historical transaction network admission retains its existing path.

## Qualification and remaining integration

Source and new tests were introduced together; no initial failing-test claim.
Three new component cases cover admission/cache publication, refresh, typed
selection, wrong root/body/cache refusal and a missing proof validator. Their
execution and fresh ON/OFF full-build qualification are pending.

This is an intermediate production interface change toward compact-node support.
The concrete selected owner still requires the full durable/live coin store and
forest. Compact catalog-backed transaction input views, compact startup, proof
acquisition/readmission across changed parents, mining proof assembly and all
configured consumers remain required. Empty or missing caches do not certify
compact spendability. Existing full-owner/stateless startup guards remain.
Network handler compilation is distinct from executing the P2P transport. No
whole-node, crash, release, platform or deployment qualification is claimed.
