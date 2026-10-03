# Mempool maintenance under one pool owner

Confirmation, expiry and size maintenance already acquire the pool mutex. They now use the existing lock-held removal, size and eviction helpers. Confirmation preserves unconfirmed children whose parent moved into chainstate. Expiry and size eviction capture the removed transaction's entire descendant branch from actual recorded inputs before removal, so children cannot remain after an unconfirmed parent is discarded.

The surviving coin overlay is rebuilt once after confirmation or expiry, using the shared outputs-before-spends helper. That helper preserves the existing confidential flag and commitment fields. Clearing the pool also clears the reverse dependency index.

Three patched-path fixtures use actual signed transparent transactions and isolated ChainDB/consensus UTXOs. They check parent confirmation with a surviving child and usable overlay, expiry followed by fresh admission, and size eviction of a complete branch followed by clear/refill. No unsafe-original, deadlock/race reproduction or synchronization-removal controls are run. Existing deadlines and policy limits remain.

This does not provide allocation-failure rollback for all maintenance APIs, fee-estimator/scanner ownership, independent CT admission coverage, all-consumer notification, load or release qualification. Mainnet activation is unset.
