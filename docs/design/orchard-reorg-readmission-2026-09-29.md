# Retained reorg readmission

Locally qualified as part of the combined typed block and wallet service integration batch.

The selected chainstate service reads one existing durable reorg intent, checks its origin against the canonical event log, and captures only the matching committed disconnect prefix. Callback counts do not authorize recovery. Blocks are processed from parent to child with their original transaction order; coinbases are excluded. Complete checked capture and operational bounds precede admission.

The actual current mempool service remains owned throughout recovery. Each transaction passes its ordinary family validator with relay disabled. Exact duplicate bodies are observed explicitly; other refusals and interrupted attempts remain retryable from unchanged retained plans and canonical events. A returned intent cursor is an observation, never a consumer checkpoint or deletion permission. Empty, partially committed and later reconnected plans do not fabricate completed work. Repeated attempts may observe changed canonical state or pool membership.

This explicit service operation does not install the production notification provider, schedule automatic recovery, acknowledge other consumers, persist a pending-operation completion, or certify continuing pool membership. Source bounds are operational and are not resident-memory or general-history qualification. Mainnet activation remains unset.
