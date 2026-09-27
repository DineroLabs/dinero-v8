# Post-quantum key cleanup on wallet timeout

The existing timeout transition now cleanses the in-memory PQ master and clears its loaded flag alongside the encryption key, seed and private-key cache, matching explicit wallet lock. It preserves the existing lifecycle mutex and refusal while a RecoverySeed owner is pinned. No durable key, wrapper, seed, account or wallet policy changes.

Tests advance the existing timeout fields through a narrow friend and invoke the real public isWalletLocked transition; no sleep, secret dump or production wallet is used. They verify cleared memory/flag, unchanged durable owner rows, re-unlock/reopen, unlimited unlock preservation and existing recovery-pin refusal until release. This is cleanup qualification, not a claim of previous key disclosure, universal accessor deadline enforcement, complete memory erasure, inventory completeness or release readiness. Viewing-key policy remains unchanged.
