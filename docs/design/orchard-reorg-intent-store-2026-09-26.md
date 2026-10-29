# Service-owned durable reorg intent

Best-chain activation and invalidation already prepare complete typed reorg plans
before rollback. The shared service preparation now synchronously stores that
plan before returning a usable transition. Consumer readiness remains mandatory.
The record preserves both paths in execution order, the selected network/profile
and the per-block outbox head at preparation. A provider refusal writes no intent.
A storage failure returns no usable transition and finishes the prepared consumer
handoff as incomplete. Canonical rollback has not started at that point.

## Retention and recovery

An immutable sequence record and checked head share one synchronous RocksDB
batch. That batch contains recovery metadata only. Cancelled attempts remain:
zero returned successes cannot prove zero durable transitions. Neither a completed
callback nor an unchanged current tip authorizes deleting an intent. A cycle of
rollback and reconnect may return to the same tip while still requiring wallet
reconciliation. The separate atomic block outbox records committed Orchard
transitions; historical transitions still require canonical reconciliation.

The reader restores one intent per call using a sequence/digest consumer cursor.
It needs no source flatfile or transaction index. It checks checksums, profile,
record order and predecessor links, full framing, typed body identity and path
ancestry. Orchard bytes retain their proof suffix; historical entries retain the
existing typed Block serialization, not every historical noncanonical wire form.
The profile discriminator must agree with selected activation by height.

A plan is limited to 2,048 total entries and 64 MiB including its serialized
framing. These are operational refusal bounds, not consensus reorg-depth rules.
Decoded objects and intermediate copies use additional memory. No partial plan
is returned or stored. Digests check local consistency; they do not authenticate
an adversarial database or establish historical proof/script/PoW validity.

The service checks the current durable tip against the plan origin before
writing. The selected activation lock must exclude reentrant canonical writes
through the entire preparation and execution. If a recovery-metadata write
reports failure, the plan may still be present; consumers must reconcile it.
Unlike a failed canonical-state write, no in-memory canonical publication has
started, so this failure can refuse the attempt without advancing the chain.

## Still required

This installs intent persistence in the real service preflight. It does not
install a production notification provider or consumer acknowledgements. Wallet,
mempool/readmission, cache/oracle/relay/longpoll recovery, consumer cursor commits,
startup readiness, retention/backpressure and load qualification remain open.
The provider contract and activation-history refusal stay in place. The release
remains inactive and unqualified until those paths work together in real nodes.
