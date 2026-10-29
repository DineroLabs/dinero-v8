# Canonical Orchard catalog integration

Status: implementation under qualification. Mainnet activation remains unset.

The prepared canonical writer enrolls the independently completed parent catalog and extends it in the existing chainstate batch. Immutable records are indexed by exact block hash; the existing canonical chain tip selects the record. There is no second advancing catalog tip. Records bind network, genesis, branch, activation, maturity-leaf rule, height, block, parent, work, both catalog roots/counts, the real verification stump, the exact previous record digest and conventional undo digest. Encodings have a checksum and strict stump framing. Checksums detect local inconsistency; they do not establish adversarial historical authority.

At activation, actual detached service preparation supplies the private completed catalog only after the selected-parent comparison. Descendants require an enrolled record. Actual service and startup reindex transitions require enrollment. Existing direct writer component calls retain their narrower stateful behavior when neither parent nor child has a catalog. They do not certify compact ingress or historical completeness. Once records exist, these calls also check them; a missing member cannot silently downgrade that enrolled transition.

The writer inserts every transaction ID, including fully spent transactions. It removes exact legacy creation metadata for spent legacy coins, rejects unexpected legacy entries for modern coins, and inserts surviving legacy outputs when Orchard activation precedes the maturity-leaf cutoff (as on regtest). Persistent removal preserves retained roots and refuses missing keys. Nodes and both records share the existing batch with coins, Orchard, retirement, journal and tip; indexed callers also include body/undo locators and outbox. Before commit the writer rechecks captured record bytes and the exact durable canonical-tip before-image. Disconnect checks the child, parent, both stumps and exact undo digest before the existing batch restores the canonical tip. Retained records must match exactly on reconnect.

New serialized fixtures cover enrollment/abandonment, changed-tip commit refusal, required-owner refusal, legacy creation, disconnect/DB reopen/reconnect, corrupted record refusal, bounded-depth persistent removal and strict complete-stump framing. This is not process restart, compact/CSN ingress, capacity qualification or proof of all spend patterns. Startup and retained-undo audits now compare catalog records, domain, work, verification stumps and predecessor/undo digests. Startup reindex independently reconstructs the catalog from the actual historical bodies before its selected mutex, compares complete coins/forest and retirement, and enrolls through the same canonical writer. These changes are under fresh qualification. Authenticated compact candidate access and compact publication remain required; the CSN guard remains. No production activation, excluded tests, races, churn or IPC probes are part of this change.

Qualification chronology: the first completed ON run built the full daemon and passed seven of eight selected CTests. The new enrollment fixture incorrectly expected one added legacy coin; the actual block has four canonical outputs, including its commitments. The corrected fixture checks all four against durable coin amounts/scripts and exact catalog metadata. No production coin or catalog behavior was changed for that correction. Fresh qualification remains required.

The added actual-service fixture covers a legacy funding spend, modern spend and same-block child, preserving historical transaction IDs even when all outputs were spent. It exercises isolated reindex reconstruction and candidate reopen, plus canonical invalidate/reconsider. Missing current/parent records, checksum damage and an incorrect predecessor digest must refuse startup and retained-undo audit. Restoring bytes does not clear the startup safe-mode latch. The reindex implementation currently performs an additional independent parent replay; resource/load qualification remains open. No whole-node, compact ingress or release qualification is inferred from these component cases.

## Full reindex fixture work (qualification pending)

The first startup/reindex qualification built the full ON daemon and replay target,
but the new service case failed when normal full reindex checked height-one PoW.
Its inherited admission fixture used synthetic unsolved headers. Nine other CTests
passed; the failing case stopped before reindex completion and subsequent refusal
checks. OFF and sanitizer qualification did not run for that revision.

The fixture now explicitly enables the existing isolated regtest PoW profile,
computes each historical header's required ASERT difficulty from its ancestry, and
mines its nonce after the Utreexo commitment is complete. It also mines both real
Orchard templates without changing their transaction bodies or commitments. The
production PoW/reindex checks, full-mode setting, and all original assertions remain.
Other admission fixture callers retain their existing default setup. Fresh ON/OFF
and sanitizer qualification remain required; this correction is not a pass claim.
