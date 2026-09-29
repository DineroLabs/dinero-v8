# Complete HTTP RPC requests and retain wallet ownership

Locally qualified.

The live HTTP server reads the complete header block and the declared body before dispatch. Header fields are parsed within that header block, names are ASCII case-insensitive, and Content-Length permits surrounding HTTP whitespace. The one-request connection format refuses duplicate lengths, transfer coding and surplus bytes. Headers retain an 8191-byte bound and bodies retain the existing 2 MiB transport bound. One ten-second receive budget covers the complete frame; socket shutdown still interrupts reads.

The existing per-IP token decision is captured once before connection enrollment. A refused request remains in the same tracked connection lifecycle while its bounded frame is consumed and a complete JSON HTTP429 response is sent. It never invokes authentication or RPC dispatch. Connection/handler concurrency ceilings and token capacity/refill rates are unchanged. Connections over the existing concurrency ceiling retain the separate early503 path.

Three framing component cases exercise valid segmented live requests, authentication on complete requests, and a captured refusal through the real connection reader over an isolated socket pair. The latter does not load-test or independently qualify token-bucket scheduling. Existing handler-drain/pool-owner and daemon checks remain required. No unsafe original, stress reproducer, copied omission or synchronization-removal control is used.

This change does not establish the cause of earlier Linux connection-reset or generation failures. It does not qualify Windows, general HTTP compatibility, sustained load, production notification installation or release readiness. The existing larger JSON parser limit does not raise the 2 MiB transport bound.

Unified HTTP RPC dispatch also retains the exact WalletService and WalletManager through the existing WalletUse owner until result/error construction finishes. A closed configured wallet refuses before the unified handler runs. This lifetime owner does not select/pin an active wallet session or hold wallet/SQL/chain locks. Reserved legacy daemon controls stay on their existing path. Three additional live-handler cases use actual WalletService::Init, wallet creation and SQLite to check exact pointer ownership/aliases, exception/error release and closed-owner refusal without destructive concurrent shutdown.

Authorization extraction is bounded to the completed header block, folds only ASCII header names, trims ordinary header spaces/tabs, and refuses ambiguous duplicate credentials. The authentication dispatcher and credential verifier remain unchanged. Positive authenticated requests cover both ordinary and mixed-case header names/spacing; the unauthenticated request still refuses. No crafted bypass or unsafe-original reproduction was executed.
