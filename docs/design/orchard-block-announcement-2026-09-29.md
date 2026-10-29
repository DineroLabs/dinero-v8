# Typed canonical block announcements

Locally qualified as part of the combined typed block and wallet service integration batch.

The typed RPC acceptor, queued P2P ingress and mining.submit hand a checked current canonical tip to the existing inventory transport after releasing selected chain locks. Mining keeps its outer parent guard through acceptance and job consumption, then explicitly releases it. The capture refuses an outer selected lock, a wrong or unavailable owner, inactive profile, mismatched live/durable/validated/coin tip, unreadable exact body, or unavailable checked delivery head.

Announcements use inv/getdata with the exact hash; they do not coerce typed transactions into historical compact blocks. One successful process-local handoff is remembered by selected service and canonical event sequence/digest. Canonical reconnect of the same hash has a new event cursor. A held pending flag prevents recursive callbacks; no relay mutex crosses transport. Callback failure leaves canonical acceptance intact and allows explicit retry. A successful handoff is not peer receipt, durable exactly-once propagation, or continuing canonicality. No unbounded announcement history is created.

No P2P socket execution, periodic retry/durable announcement queue, typed compact reconstruction, whole-node provider installation, mainnet activation or release claim. The existing regtest announcement suppression hook remains honored. External dependencies and platform/load gates retain their separate scopes.
