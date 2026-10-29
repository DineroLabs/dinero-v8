# Typed pool effects and cache publication

PreparedPoolTip accepts exact typed connected-block effects and prepares the
pool, bridge proof-cache invalidation and relay refresh state together. The
Historical block adapter delegates to this same path. The caller holds selected
chain ownership, retains service lifetime, and publishes only after canonical
commit. Preparation can fail or be abandoned without publishing any consumer.

Publication uses the existing nonthrowing owners, then releases their locks.
Outbound proof refresh runs afterward and may fail ambiguously; that failure
does not undo the already committed pool/cache state or permit a second send.
Absent bridge or relay consumers retain their existing policy.

The tests cover Historical adapter parity and structural mixed-family nullifier
conflicts with a surviving ordinary transaction, an actual bridge proof cache,
and actual relay refresh tracking. They verify abandonment, preparation failure,
post-publication callback observations and ambiguous sends. Synthetic envelopes
are not claimed as authorized/admitted Orchard transactions. Refresh of a
surviving Orchard body is not qualified here.

This is a local pool/cache owner. Production RuntimeBlockNotifications still
needs all configured consumers and durable reorg/readmission ownership before
installation. Orchard admission, typed mining, wallet completeness,
shield/send/unshield, whole-node/platform/load qualification remain open.
Mainnet activation remains unset.
