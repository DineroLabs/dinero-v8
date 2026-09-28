# CSN proof receipt and staging acknowledgment

A stored competing-branch body does not establish that its queued ordered proof
was validated. The stateless scheduler now retries an expired receipt at its
existing interval until the ordered worker acknowledges successful proof
validation and durable replay/delta staging for that exact hash. An acknowledged
competing receipt waits for normal chain activation, preserving whole-branch
assembly. Staged ancestors are skipped when selecting the next proof retry, so
a later unacknowledged descendant cannot be hidden behind a staged ancestor.
Forward receipts retain the existing retry behavior.

The acknowledgment applies only to a current RECEIVED entry in stateless mode.
It cannot create an entry, revive INVALID, change canonical membership or
replace proof validation. Exact-hash rescans retain it; hash replacement and
explicit retry clear it. The actual reorg worker acknowledges only after its
existing successful proof, checked durable sidecar write and a usable stored
body probe after locator persistence. A failed locator write cannot suppress
receipt retries. An acknowledgment
that loses an explicit-retry race may be a no-op; the new receipt needs its own
acknowledgment. No application callbacks or new lock-order edges are introduced.

Completed Linux restart-churn logs showed a competing receipt retained while
forward work advanced on another branch and later proofs repeatedly refused a
changed parent. This change closes the unacknowledged-receipt stall path; it does
not establish that all CSN churn failures have the same cause. Existing activation,
proof, invalidity, timeout and assembly guards remain. Benign receipt lifecycle
checks and real restart/churn qualification are required; no unsafe originals or
synchronization-removal controls are used.

This is scheduler liveness after lost pending proof work, not independent
historical validation or Orchard release readiness. Mainnet remains unset and
production Orchard notifications remain absent.
