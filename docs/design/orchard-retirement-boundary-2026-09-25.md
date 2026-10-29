# Selected retirement boundary record

`DeriveSelectedLegacyRetirementUnderLock` constructs the boundary record from
one selected archival state. It accepts a database, optional archival flatfiles
and a local epoch-work budget. It accepts no amount, epoch, network identifier
or frozen-state root from a wallet, RPC or proposed block.

1. Derive the latest legacy epoch's public flows using
   `DeriveSelectedLegacyPoolAccountingUnderLock`. Its validated-history and
   selected-writer-lock preconditions remain mandatory.
2. Derive the network, genesis, branch and activation from fixed selected
   parameters. Require that the accounting result names their activation parent.
3. Require absent current Orchard state and absent retirement receipt at this
   first boundary. Missing rows are distinct from storage errors.
4. Read the frozen frontier, anchors, nullifiers and legacy tip marker. Decode
   and reconstruct SHR1, check marker height/hash/tree/counts and nullifier
   uniqueness and epoch range. Never use a caller-supplied root or count.
5. If historical state commitments are active at the parent, authenticate its
   body and require exactly one valid **legacy DNRS v1** commitment equal to
   that reconstructed SHR1. This preserves the historical encoding; the first
   Orchard block uses the separately specified composite DNRS v2 encoding.
6. Recheck selected tip identities and return the record. No writes or partial
   result occur on failure.

The frozen-content derivation is shared with full staged connect/disconnect
verification. Those checks compare the derived contents with their immutable
retirement receipt. Retirement storage and undo remain in the existing outer
ChainDB batch, and Orchard starts at zero with no retired-value credit.

## Limits and next integration

This factory does not establish historical proof soundness, unspentness, fee and
coinbase conservation or snapshot provenance. These are obligations of the
already validated history, not facts proved by tip markers. Where historical
DNRS is inactive, independently validated state remains the content authority.
An active parent's commitment additionally binds frozen contents, not the new
retired-value calculation or previous retired epochs.

This is the stateful archival path. Missing/pruned history and unsupported
monetary amounts are local failures, not reasons to mark a peer block invalid.
CSN/pruned-node accounting sources and loaded scan capacity are still open.

The factory returns a persistence record, not a general cryptographic
certificate. The full staging API still accepts a record under its documented
caller obligation; runtime wiring must use this factory or an independently
qualified authenticated source, in the same held view. No production caller is
enabled by this change. Mainnet activation remains unset.

## Qualification scope

The accounting test now combines selected public-flow derivation with actual
serialized frozen contents and a matching parent commitment. It checks the
derived amount, epoch, domain, parent, tree and nullifier identity, rejects
marker inconsistencies, altered anchors and missing active DNRS, and verifies
read-only reopen. These generated stores are component fixtures, not proof-valid
historical replay. Existing staged connect, disconnect and process-exit tests
exercise the shared frozen-content checker after the refactor.

Local full daemon build and 47 selected CTests passed (32 mandatory Orchard
registrations, 15 compatibility lanes, and 21 internal Rust tests). ASan/UBSan
passed with 65 linked project C++ translation units for the factory and 68 for
shared staging, including 12 fresh-process commit/publication cases. Removing
the derived amount assignment or parent DNRS comparison makes the factory
regression fail. Removing the current DNRS check, frozen-content comparison or
retirement staging makes the corresponding existing regression fail. Rust and
external libraries are uninstrumented; macOS leak detection is disabled.
These are local working-source results; exact-head Linux is still required.
