# Owned proof completion status (draft)

The finish-spend and finish-shield paths authenticate the existing request and durable reservation before observing its process-local proof job. An unavailable proof reports the existing human-readable error plus `error_code: proof_not_ready`, `proof_state`, and `reservation_retained: true`.

States are `queued`, `running`, `cancel_requested`, `failed`, `cancelled`, `missing`, `result_unavailable`, or `unavailable`. Only the executor observation can report queued/running. Missing after restart or wallet-session change is not evidence of work still running. These observations are snapshots, not guarantees that a job will finish or that a later call will observe the same state. No state authorizes automatic requeue, reservation release, new request IDs, replacement payments or repeated admission.

Ready transactions continue to use retained signed bytes. This change does not implement finish-by-ID, incoming history, activation status, a status polling endpoint, or complete typed errors. Other errors retain their current contract. Runtime qualification is pending. The existing actual RPC missing-owned-job case now explicitly checks the structured refusal and unchanged durable state/no submission; the shared generic error helper remains unchanged. Other executor-state branches require additional actual runtime coverage.

An additional actual service case uses the executor in its default idle state, observes queued, cancels through its public API, observes cancelled, forgets the terminal job and observes missing. It requires unchanged durable reservation and no mempool admission throughout. This uses no worker, timing assumption or synchronization override. Runtime execution remains pending.

Same-service wallet reopen may retain a job owned by the previous session. That is an ownership refusal, not a missing-job observation. The new RPC case first requires that refusal, then explicitly removes the old completed process-local result through the executor API before requiring a typed missing response. It does not simulate a whole process restart or weaken the job binding check.
