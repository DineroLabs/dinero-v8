# Orchard account fixture initial identity

The account-delivery fixture now creates its real wallets through BIP39 recovery using a fixed public test mnemonic. Its account keys, standalone encrypted snapshots and child-process snapshots use the corresponding seed. Enrollment explicitly compares the wallet recovery seed with that fixture seed.

Previously the fixture generated a new wallet and then replaced its seed before enrolling account snapshots. That left its seed-bound initial-owner record associated with the original seed, so staged unlock correctly refused authentication later in the scenario.

Only fixture identity setup changes. Existing delivery, SQL rollback, retained-parent undo, repeated reconnect, reopen, lock, legacy-format refusal, self-link refusal, historical observation and fresh-process commit checks retain their assertion bodies. Production authentication checks, registrations and deadlines are unchanged. The fixture still explicitly enrolls synthetic baseline snapshots; it does not prove production baseline completeness.

Qualification is recorded privately with exact source and execution evidence. This test-only repair does not complete authenticated inventory, seed replacement, PQ account ownership, whole-wallet recovery or release readiness. Mainnet activation remains unset.
