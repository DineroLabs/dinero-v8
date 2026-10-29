# Orchard startup undo coverage

`ChainstateService::VerifyActiveChainUndoCoverage` now walks retained Orchard
bodies before entering the historical body reader. The complete audit holds the
activation mutex. It requires the selected stateful persisted tip, validated
tip, forest marker, Orchard state and retirement marker to agree, then restores
a private forest from the retained checkpoint and deltas. This works at the
early startup call site, before live service memory has been restored.

Each selected Orchard step checks the indexed body against the exact embedded
commit body, including the Utreexo suffix. It requires usable body and undo
locators, rejects failed header status, compares flatfile and embedded
conventional undo, and verifies active height/transaction indexes. The retained
Orchard undo links current and parent state. Mandatory local commit records bind
both to the selected domain, headers, work and private forest. The audit reverses
the forest delta and binds conventional undo coin values/scripts/heights to its
leaf identities. It also reconstructs the compact filter, including its element
count. Shared internal checks are used by the actual staged disconnect path.

The walk changes only its private forest and state cursor. It never applies an
undo batch or publishes a tip. The requested window bounds the steps; zero
retains the existing meaning of no explicit step limit. It crosses the first
Orchard block into the historical audit with the historical behavior preserved.
A missing or inconsistent Orchard record fails the audit, enters safe mode and
requests operator review through the recovery marker when a datadir is present.
An inactive/unsupported Orchard build or unsupported stateless profile cannot
silently take the legacy success path for stored Orchard state.

This is retained disconnect-material coverage, not historical consensus replay.
In particular it does not re-verify proofs/scripts or independently reconstruct
historical nullifier ownership. Current coin rows cannot be compared against an
older block because descendants may already have spent them. The separate
mandatory tip-local audit remains necessary. Pruned retention policy, actual
production connect/disconnect, complete Start/reindex and stateless support
remain unfinished. Mainnet activation remains unset.

The independent `OrchardServiceUndoCoverage` registration uses the real service
on generated full-state stores, with checkpoint and delta restoration cases. It
checks a two-block window, an ancestor outside versus inside that window,
missing undo, inconsistent coin/delta data, exact body bytes, profile and tip
mismatch, safe-mode entry and unchanged logical database rows. The old service
route fails the new regression. These fixtures are not a running-node or release
binary qualification.
