# Check descendant policy for every surviving ancestor

A new transaction adds a descendant to every unconfirmed ancestor. Admission now uses the existing complete ancestor traversal to check each ancestor's descendant count and byte limits. Both test-only and actual admission use the same surviving pool after the validated replacement set is excluded. Existing count, size and replacement fee rules remain unchanged.

Two bounded patched-path fixtures use real signed Taproot transactions and an isolated ChainDB/consensus UTXO set. They verify that a non-direct ancestor at its descendant limit refuses another branch extension without changing pool entries, spent-output state or callbacks; a valid replacement frees capacity and permits the extension. No original vulnerable path or omission control is executed.

This is policy-bound qualification, not whole-pool scaling, allocation-failure rollback, complete atomic admission, notification-provider installation, Orchard admission or release readiness. The existing traversal scans remain; mainnet activation is unset.
