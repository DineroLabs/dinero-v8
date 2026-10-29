# Detached catalog observations during provider delivery

The operation-listing RPC restores a complete authenticated catalog outside wallet ownership, then authenticates a fresh snapshot and checks that the capture still matches before returning anything. Ordinary wallet delivery can update durable account revisions during that interval. A private reproduction of an actual HTTP-cycle failure identified the final aggregate snapshot comparison as the refusing check after the activation block was invalidated.

The catalog reader preserves fail-closed behavior. Content differences between authenticated snapshots under the same wallet instance and identity now throw the distinct `CatalogChanged` exception. Identity and viewing-key mismatches, SQL errors, unavailable credentials, invalid envelopes and restoration failures retain their original failures. No partial catalog is returned.

`wallet.orchard.listoperations` catches only `CatalogChanged` and attempts at most four complete reads. Every attempt captures a new immutable replay source before acquiring wallet/SQLite ownership, authenticates all current and retained owners, restores the captured payloads outside those owners, and performs the final full comparison and checked commit. Only a successful attempt publishes operations and its matching captured-source checkpoint. Exhaustion returns an error. The reader neither synchronizes the wallet nor writes account state.

The existing authenticated archive-replacement fixture now requires the specific content-change exception. Added cases require wallet locking during detached restoration to remain a nonretryable failure and an actual RPC SQL denial to cause exactly one denied read, with no partial response or durable change. Existing HTTP assertions and deadlines remain unchanged.

This does not detach all composite callers or writers, establish history capacity bounds, or establish release readiness. Public workflow dispatch remains subject to the existing hold. Mainnet activation remains unset.
