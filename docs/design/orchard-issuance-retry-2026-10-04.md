# Bounded account issuance retries

`wallet.orchard.createaccount` and `wallet.orchard.getnewaddress` retain the request's original wallet session and lifetime handles. A retry recaptures safe mode, the immutable replay source and its release profile before authenticating/restoring the current catalog. Source capture runs outside wallet SQLite/key ownership.

Only `OrchardAccountDelivery::CatalogChanged`, the typed comparison of authenticated captures, permits retry. At most four attempts occur. Every refused attempt rolls back before returning to the retry helper. Ordinary session, identity, source, SQL, decryption and restoration errors propagate without retry. Another request creating the same account causes the subsequent existing-account refusal; it never authorizes replacement.

The response is constructed only after the issuance transaction has committed. A discarded prepared address is never returned, and the retry does not advance its durable issuance counter. Existing low-level catalog guards and the separate listoperations retry remain unchanged.

The motivating normal integration run reached the coupled activation boundary, then the second account creation returned a catalog-change refusal. The exact background interleaving was not established. The new serial tests intentionally make real, authenticated changes to another account between capture and recheck. They are deterministic component tests, not concurrency or HTTP transport qualification. The unchanged coupled HTTP integration still requires a fresh successful run.

This bounded retry does not guarantee success under sustained catalog changes and is not an overall readiness certificate. Parent history capacity, whole-node/platform qualification, rollout and mainnet activation remain separate gates.
