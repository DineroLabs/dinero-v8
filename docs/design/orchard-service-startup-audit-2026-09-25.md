# Orchard service startup audit

The actual `ChainstateService::VerifyConsensusJournalAtActiveTip` now routes
Orchard state through its mandatory tip audit before the optional legacy journal
check. `ActivateBestChain` returns when verification fails and leaves its startup
verification flag unset. The next attempt must verify again.

## Preconditions and checks

The service holds its activation mutex, uses the selected network profile and
requires a restored stateful consensus view at the active index's height/hash.
A stored Orchard state or retirement receipt cannot be reinterpreted as legacy
by choosing inactive parameters. A post-activation persisted tip also requires
restoration even when the in-memory active index has not caught up and both
state rows are missing. Missing runtime support, unsupported CSN restoration, missing state,
invalid configuration or storage errors refuse activation through safe mode.

The strict indexed flatfile reader checks the body identity. Persisted metadata,
header fields, availability flags, chainwork and locators must match the active
index. The flatfile undo must equal the conventional undo in ChainDB. The exact
flatfile body must equal the committed embedded body, including the Utreexo suffix
outside the transaction Merkle commitment. Neither a good database body nor a
matching header can hide an incomplete flatfile record.

With the forest held for reading, the existing full reverse-staging audit checks
the restored forest, selected tips, mandatory Orchard commit record, coins and
undo, frontier/pool, nullifier owners, anchor references, active transaction
indexes, compact filter, DNRS v2, frozen legacy contents and retirement records.
Its batch is abandoned. It does not repair, rewind or write chainstate. Failure
is local startup inconsistency, not a peer consensus-invalid verdict.

Historical stores without Orchard state retain the old optional journal rules.
Safe-mode reporting also handles early startup without a logger.

## Regression scope

`OrchardServiceStartup` links the actual service to the existing generated
full-chainstate fixture. A test-only callback runs after durable commit, database
reopen and forest restore, with and without a checkpoint. The shared fixture
contains an honest Orchard funding transaction and a same-block transparent
child. Existing unused daemon/mempool link stubs remain; this is not a running
node or a production connect test.

The original service accepted a missing mandatory commit record with the legacy
journal option disabled. The regression requires refusal, preserved startup gate,
and unchanged tip. Positive cases cover both legacy journal option settings.
Negative cases cover stale memory, empty forest, missing/bad locators, mismatching
undo, incomplete flatfile proof suffix, wrong branch, inactive parameters and
unsupported CSN restoration. Both historical database layouts remain accepted.
All logical database rows are compared before/after the audit scenarios.

The root Orchard workflow requires this independent test name and executes it.
A passing inventory is not execution evidence; actual logs are required.

## Remaining integration

This is the real activation-time startup gate, after the service has restored its
active index and memory. It is not complete `Start`, replay, reindex, pruned/CSN
restore, historical consensus validation, admission or connection. Earlier
startup readers and typed connect/disconnect/publication/notifications still need
integration. No network activation is selected. A passing component/service
fixture is not working shield/send/unshield or release qualification.
