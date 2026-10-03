# Selected-network Orchard profile

This component prepares the runtime routing boundary. It does not enable a
production caller, choose a public activation height or choose a production
signing branch ID.

## Configuration

Every shipped network starts with `orchard_activation_height = UINT32_MAX` and
`orchard_branch_id = 0`. The pair is inactive even at the sentinel height. A
scheduled profile requires a positive supported height and an explicit nonzero
branch ID. Unknown network names fail configuration validation.

Public networks require the Orchard height to equal the joint release boundary,
which already couples compact proofs, timing and service policy. Regtest may
exercise an independent Orchard boundary. `ConfigureOrchardRelease` updates a
copy and publishes only after all prerequisites pass, so an invalid request
cannot partially change the timing or compact switches. It is a source/test
helper, with no RPC or command-line activation interface.

Network selection validates the configuration before publishing it. Parameters
must remain fixed while a caller holds the selected chain/writer context.

## Header context

`SelectedOrchardBlockContext` derives activation, network, genesis and signing
branch from selected parameters and candidate identity from the header. It
returns no context before activation. Invalid local configuration is a lookup
error, rather than evidence that a peer sent an invalid block.

The staged contextual header gate independently requires the caller's activation
height and signing branch to match this selected profile. The existing exact
network/genesis, parent ancestry, checkpoint, timestamp, ASERT and PoW checks
remain in place. A different nonzero branch ID is rejected as well as zero.

## Scope and qualification

The expanded `OrchardHeader` test covers all three inactive network defaults,
transactional invalid configuration, exact activation and rewind boundaries,
public schedule coupling, isolated regtest scheduling, selected context fields,
and disagreement between selected parameters and caller context. Existing
competing-branch, timing, checkpoint and PoW cases still run.

This is not live transaction admission. Connect/disconnect, replay/reindex,
wallet services, mempool, relay and mining still need runtime integration and
real-node qualification. No public schedule is authorized by these tests.
