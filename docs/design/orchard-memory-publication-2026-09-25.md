# Prepared in-memory publication for Orchard integration

The authoritative block batch already stages coins, pool/nullifiers/frontier,
forest/undo, body/indexes and tip together. The runtime must also advance its
`ConsensusUTXOSet` only after that batch succeeds. Calling allocating coin
insertion or forest copying after the durable write is not a safe publication
strategy: an allocation failure could leave memory behind the database.

## Ownership and ordering

`PreparedUTXOPublication` is a move-only, single-use memory transition. The host
holds its selected activation/writer lock across this entire sequence:

1. Validate all block obligations and stage the authoritative batch.
2. Prepare exact before/after coin changes and the owned next forest. Verify the
   current tip/root and all touched coin fields, including confidential flags
   and commitment bytes. Reject duplicates and unexpected existing outputs.
3. Allocate insertion nodes and reserve destination bucket capacity before the
   commit. This changes capacity only; existing logical coins remain unchanged.
4. Recheck the prepared view, then commit the corresponding outer database batch.
5. On success, publish using preallocated node handles and a forest move. On
   failure, discard the prepared publication; no live coin/forest/tip changes.

Connect and disconnect use the same primitive with opposite checked patches.
The full staged disconnect now returns its reverse coin patch together with the
restored forest. It derives removals from the exact stored body and restores
from conventional undo, then matches both sets to the durable forest delta.
Each restored coin must also exist as its creation-height leaf in the restored
parent forest. Shape-valid undo with a changed value is therefore a local
corruption error, with an empty abandoned batch. The partial coin/state helper
does not perform this forest binding and is insufficient for runtime publication.
This uses the historical creation-height leaf rules; it does not add fields to
older leaf encodings or independently revalidate the entire origin history.

After restart the host can use that returned patch; it must not reconstruct it
by retaining or reversing an old in-memory connect result. Same-block transient
outputs remain absent from both the durable and in-memory rollback.

It is not a block validator and does not prove that the caller committed. The
host must supply patches from validated connection or exact checked undo, never
from peer-provided claims. It cannot serve as a snapshot import or tip repair.

The publication acquires the existing exclusive forest lock, changes the coins,
moves the forest and updates tip/height. Coin and tip readers remain subject to
the activation lock; this does not make previously unlocked coin readers safe.
No database/activation lock may be acquired while the leaf forest lock is held.
The successful publication path allocates no C++ objects. A broken ownership or
lock contract after durable commit terminates instead of reporting success and
continuing with inconsistent memory; restart must restore authoritative state.

## Resource behavior

Preparatory memory is proportional to touched coins, plus the already prepared
next forest. The implementation does not copy the whole UTXO map. A bucket-table
expansion may allocate before commit when capacity is insufficient. The host
should transfer ownership of its prepared forest; passing a const forest copies
it before commit. Runtime forest ownership and loaded-node costs still need
qualification.

## Tests and remaining work

A mandatory root test checks exact coin/context mismatches, abandonment,
preparation allocation failure, move ownership, and connect/disconnect while
ordinary C++ allocations are disabled during publication. The real Orchard
staging fixture exercises the same API around successful outer ChainDB writes,
including ephemeral same-block outputs, with and without forest checkpoints.
Its persistent-state crash checks remain in place.
Fresh-process checks now restore all current coins and the forest from the
temporary database, prepare both directions, and terminate at twelve boundaries
across checkpoint/no-checkpoint cases: before commit, after commit before memory
publication, and after publication, for connect and disconnect. Published memory
is compared field by field with the committed database, including coin count,
scripts, amounts, creation metadata, tip and forest commitment. The parent then
reopens and compares the complete selected persistent state. These are generated
store tests, not a whole-daemon lifecycle claim.

This is a runtime integration prerequisite, not a new production caller.
ChainstateService connect/disconnect, startup restoration, DNRS/legacy-state
obligations, typed relay/mining and wallet services remain to be wired and
qualified together. No network activation or deployment is made here.
