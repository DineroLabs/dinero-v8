# Staged Orchard state-commitment encoding

Status: encoding prerequisite for runtime integration, not an activated rule or
a complete Orchard state commitment. All existing callers retain DNRS v1.

## Explicit version boundary

The existing 39-byte coinbase output format remains:

```
OP_RETURN | push 37 | "DNRS" | encoding version | root bytes[32]
```

`StateCommitmentEncoding::Legacy` is 1. Every existing one-argument builder,
parser and lookup delegates to exactly that version. Historical SHR1 preimages,
height selection, snapshot rules and accepted legacy output values are unchanged.

The staged `Orchard` encoding is 2. Callers must select it explicitly; the codec
does not consult chain height or enable Orchard. Each parser rejects the other
version, and unsupported enum values are rejected rather than treated as a
future compatible format. A block containing both versions has two DNRS outputs
and fails the exactly-one lookup. Malformed tagged outputs also count when
checking duplicates.

The v2 lookup requires an actual coinbase and one zero-valued transparent output
without confidential commitment, range-proof or nonce fields. Its bytes must
match the canonical script exactly, with no suffix or alternate push encoding.
Root bytes keep their existing raw `uint256` array order; displayed hexadecimal
order is not used. Version 2 here names the **coinbase script encoding**, not the
upstream Orchard bundle or circuit version.

## Obligations before a live caller

This codec accepts a supplied digest. It does not establish how that digest was
computed or prove that it represents the selected chain. No runtime path may
start supplying legacy SHR1 or the Orchard note-tree root alone in its place.

The composite state preimage still needs to bind the selected network/profile,
the retirement boundary ancestry and authenticated accounting of frozen legacy
state, and the current Orchard state including pool, nullifiers and anchors.
Its construction must avoid a coinbase/Merkle/header circular dependency: the
current block hash cannot be a field of the digest committed inside that same
block. The parent identity can be used for ancestry binding. Snapshot coverage,
atomic retirement/undo, post-boundary markers, miner construction and selected
height enforcement remain required integration work.

No activation height, production branch identifier, new state preimage or
retirement amount is chosen by this change. Existing data is not migrated.

## Qualification

The mandatory Orchard root test uses literal independent v1/v2 byte vectors and
checks mutual exclusion, all truncated prefixes, trailing bytes, altered framing,
root-byte preservation, duplicate/malformed mixtures, unsupported versions and
v2 output constraints. It pins old lookup behavior alongside every new output
constraint. The existing legacy canonical, encoding, external-vector and
transition suites remain separate compatibility checks.
