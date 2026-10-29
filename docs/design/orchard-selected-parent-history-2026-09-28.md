# Independent history at the selected Orchard activation parent

The service boundary path derives retirement only after independently replaying
selected genesis through the exact activation parent. The private source owns
the selected-chain mutex throughout replay, full state comparison and accounting
derivation. It uses the existing isolated `AssumeUtxoReplayEngine`, including its
own header ancestry and contextual lock times, normal header rules, and stateful
block validation. A persisted tip or retirement row cannot replace that replay.

Every selected height must have its exact body, header and accumulated work.
Replayed coins must match every live and durable coin, including confidential
metadata. The forest commitment and leaf count, durable forest marker, shielded
composite state, frontier and anchor persistence must agree. Only then does the
existing selected-history accounting factory derive the last legacy epoch's
retired amount. The resulting record stays inside the actual prepared boundary
write; no RPC or wallet supplies an amount or history certificate.

`ChainDB::forEachUTXO` now refuses malformed keys, truncated or trailing coin
encodings, noncanonical flags and unrepresentable heights. It preserves valid
older coin encodings. Visitor exceptions propagate; intentional early stopping
remains supported and is explicitly not a complete inventory. The boundary
source requires complete successful enumeration and exact record count.

The existing provider, selected header, parent audit and prepared-write guards
remain prerequisites. Backend-OFF refuses the boundary source. Replay is bounded
at 100,000 blocks and 256 MiB of serialized input; exceeding either limit refuses
instead of truncating. These are operational limits, not resident-memory or load
qualification. Full replay while holding the selected writer lock may be costly.

New component checks exercise a genuine short regtest genesis history with an
empty legacy pool, agreement and refusal before retirement publication. They do
not by themselves qualify nonempty legacy epochs, the complete first boundary
connect/undo/reconnect, a production notification provider, full historical
capacity, supply accounting or shield/send/unshield. Mainnet activation is unset;
release qualification remains open.
