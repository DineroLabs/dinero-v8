# Bounded Orchard wallet proving jobs

`OrchardProofJobs` runs one proof at a time on one worker thread. The existing
Rust backend independently uses a two-thread proving pool. A wallet service must
share one executor across its accounts; creating an executor per RPC request is
not this design. Start is explicit so restored wallet state can be established
before work runs. There are no per-request worker threads.

Four jobs is the total capacity, including queued, running and retained terminal
states/results. Submission at capacity fails instead of waiting or discarding
another operation. Collecting a successful result, or explicitly forgetting a
failed/cancelled job, frees its slot. Request preparation and the service's RPC
queue still need their own bounded admission; this component cannot bound work
the caller did before submission.

## Durable handoff

The host first commits a `Reserved` operation and its ordinary wallet input locks
in the same SQLite transaction. It submits the exact owned randomized plan and
signing context against that published reservation. Message, inputs and action
nullifiers must match. A new plan for the same payment is not interchangeable.
Submission consumes the plan even when it rejects; the durable reservation is
unchanged and must be explicitly reconciled by the host.

The worker holds no queue, wallet, SQLite or chainstate lock while proving. A
successful job returns the backend's verified bundle, not a broadcastable fully
authorized transaction. The host still signs/verifies transparent inputs,
rechecks the selected chain, commits the exact `Ready` transaction with wallet
state and then performs fresh node admission before relay. `TakeResult` never
releases durable wallet reservations. Restart uses the existing encrypted
Reserved/Ready recovery rules; in-memory job state is not a new recovery journal.

## Cancellation and shutdown

| State | Cancellation / collection |
| --- | --- |
| Queued | Remove from execution queue, destroy the owned plan, retain Cancelled status |
| Running | Mark CancelRequested; discard the result when computation returns |
| Succeeded | Collect the verified bundle once; cannot cancel or forget it |
| Failed / Cancelled | Explicit Forget frees the status slot |

The proof library cannot interrupt an active proof. `RequestStop` is nonjoining,
rejects subsequent submissions and cancels queued/running work. `Shutdown` and
destruction join the worker after active computation returns. Concurrent shutdown
calls are serialized; repeated shutdown is safe. No hard kill is introduced.
This is not proof that the daemon stops within its watchdog deadline. The service
must request stop early and qualify drain time on target hardware before wiring
this executor into daemon shutdown.

Job failure reports a generic status rather than exception text containing
potential wallet data. Thread-safe query/change-wait methods expose only job
states; each wait is bounded to at most 30 seconds. No callbacks run under the
queue mutex and no live wallet/RPC caller is enabled yet.

## Tests

`OrchardProofJobs` is mandatory in the root Linux CI inventory. It checks the
four-job bound including retained cancelled entries, absent/mismatched durable
reservations, queued cancellation, a real successful proof followed by actual
transparent authorization and `SetReady`, active-result cancellation, one-worker
ordering, stop with queued work, and concurrent/repeated shutdown. Proving uses
the actual pinned backend; no executor injection can manufacture authorization.
Proof-library resource-failure injection and production service/watchdog testing
remain open.
