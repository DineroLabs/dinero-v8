# First Orchard boundary through the service

The first-boundary regression extends the real replay/service fixture from an
honest regtest genesis history. It constructs a coinbase-only Orchard candidate
with witness, filter and state commitments and an actual forest proof, stores
its flatfile locator and verified header ancestry, then invokes the real
`ConnectTip` and `DisconnectTip` entry points.

The candidate commitment uses the independently replayed parent record. The
service must derive that record again through its private history source; the
test cannot supply it to the service. Missing consumers, inconsistent durable
coins and consumer preparation refusal must leave the parent selected without
retirement publication. Successful publication must follow the durable tip.
The round trip checks first connect, disconnect below activation, database
reopen and reconnect without replacing the live coin or shielded state.

This fixture uses the ordinary regtest profile, including its configured
proof-of-work exemption, and an empty legacy pool. It does not qualify mainnet
proof of work, nonempty legacy epochs, production notification consumers,
actual Orchard transfers, shield/send/unshield, full restart, load or release
readiness. Mainnet activation remains unset and the production notification
provider remains absent. Qualification results are retained privately.
