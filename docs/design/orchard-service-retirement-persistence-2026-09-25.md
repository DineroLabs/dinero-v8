# Legacy service persistence after retirement

## Problem and behavior

The atomic retirement transition owns the frozen legacy frontier, anchor history,
nullifiers and advancing legacy marker. The service still has historical helpers
used by shutdown, notifications and snapshot recovery. Those helpers could write
from a stale in-memory cache or move the marker independently of the composite
state after the boundary.

The production helpers now acquire the activation lock and inspect the retirement
record before writing:

- `PersistShieldedState` accepts an unchanged cache as a read-only no-op. It
  compares serialized frontier/anchors, composite legacy root, tree root and
  counts to the stored receipt. Disagreement or unavailable state refuses before
  touching ChainDB or either fallback file.
- `PersistShieldedTipMarker` permits only a no-op matching the existing marker
  and retirement tip/content. It cannot rewind or advance that marker separately.
- `PersistImportedShieldedState` refuses a legacy replacement while retirement
  exists. An Orchard-aware snapshot must move the composite state atomically.

Old-schema databases have no retirement namespace and retain their historical
path. Separated databases without retirement also retain their historical path.
Undoing the actual boundary removes the receipt and restores legacy persistence.
Read errors are not treated as proof that retirement is absent.

## Regression and limits

The real `ChainstateService` loader/persistence functions run against temporary
stores and SQLite caches. A new case first failed because the marker helper
accepted rebinding to the prior height. It now checks no-op behavior, unchanged
database and fallback-file bytes, rejected marker rebinding and import, stale
memory refusal, closed-database refusal, and legacy behavior after boundary undo.
A separate old-schema case exercises all three pre-retirement write helpers.
`OrchardServicePersistence` independently registers the retirement case, and
the root workflow requires it plus the full `ShieldedStateStartup` suite by
actual CTest inventory, with execution logs retained.

This does not make the complete snapshot-import path Orchard-aware, audit all
historical state, or guard every low-level ChainDB writer. It is not production
ConnectTip/DisconnectTip integration or a whole-daemon shutdown qualification.
The source enables no activation and touches no live datadir.
