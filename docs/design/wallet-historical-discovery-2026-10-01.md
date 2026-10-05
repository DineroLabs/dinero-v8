# Historical import discovery through canonical recovery

Preparation only. These two regression cases are written against the shield
successor and have not been compiled or executed. The live qualification source
is unchanged. No initial failure, successful discovery or release qualification
is claimed.

The first case reconstructs the predecessor imported_keys record with the exact
SHA256(internal_xonly || 0x00) tweak and recorded MAIN address, even on the
isolated regtest chain. It tests both authenticated raw32 and hex64 plaintext
formats, encrypted under the fixture's actual password-derived key and stored as
binary TEXT using explicit byte lengths. The fixture's scalar is synthetic.
The production typed signing resolver must authenticate that tuple before any
attempt to discover or fund its script.

No addresses, derivation-path, modern Taproot, or watch-script companion is
created. The original address, ciphertext and label must remain byte-identical.
An actual retained ordinary payment must fund the script at height 103. Normal
canonical recovery must discover that exact output into both ordinary and index
stores. The test never inserts this output manually and requires an empty HD
path. The shield request must reserve, prove, sign, retain Ready and survive
reopen with its exact body. Actual admission and mining at height 104 validate
the historical signature; normal recovery must credit account 17 with 20,000
una. Both payload formats follow this complete sequence.

The second case corrupts a present encrypted owner after successful discovery.
Recovery must defer without changing other wallet state, and shield queueing
must refuse without publishing a proof job. This is an ordinary data-refusal
fixture, not a race, synchronization-removal or unsafe reproduction.

## Implementation still required

The existing typed signing lookup already authenticates and signs the historical
tuple. Discovery remains incomplete: recovery requires a companion address row,
script reload reads only addresses/watch_scripts, and the index schema plus
AddUTXO/GetUnspentUTXOs assume every owned coin has a path. Ordinary and index
source-domain snapshots and delivery receipts also represent that assumption.
Relaxing only one of these checks would not implement discovery.

The implementation must carry an explicit historical-import owner through
recognition, persistence, receipt binding, replay/reconciliation and selection.
It must authenticate every present import in the same checked wallet snapshot,
retain the exact original script/address and distinguish a real non-HD owner
from an unowned pathless coin. Existing pathless-without-owner refusals remain
required. Do not fabricate a path, modern descriptor, account or watch label to
satisfy the old schema. Schema migration must retain all coin metadata and
receipt invalidation guarantees, roll back on required SQL/COMMIT failure, and
preserve existing owners on conflict. Restart and reorg coverage is required in
addition to the end-to-end case written here. Deletion/backup completeness is
still a separate unresolved requirement.

The packet adds an enabled 180-second CTest without modifying existing test
bodies, registrations or deadlines. No workflow is edited or dispatched because
public publication remains held. Required later work includes actual fresh
ON/OFF compilation, execution of these cases, linked-project sanitizer
qualification, independent private evidence verification and release gate
qualification. Written tests are not evidence that these behaviors work.


## Typed index access preparation (not compiled)

The future index now persists owner kind/reference with all existing fields. Public AddUTXO refuses historical/unknown tags; a private delivery-only insertion requires its caller-owned transaction and matching authenticated script/address map. Upserts refuse a conflicting script/path/kind/reference instead of replacing provenance. Bindings and the affected row are checked.

One typed decoder now backs direct/unspent coin reads and balance inputs. It verifies SQL types, complete reads, full-width output indices, CT and accumulator metadata, and historical ownership against the private live map. Missing authorization throws instead of returning a partial vector or apparent zero balance. Existing path-only refusal assertions remain on public insertion; no fake derivation path is created. Balance arithmetic now refuses overflow. Existing maturity and CT balance conventions are preserved.

This private historical map has no publication site yet. Source capture, receipt binding, authenticated publication after coverage commit, reopen/reorg integration and the reverse companion guard still need their next implementation. Therefore no historical coin is newly spendable, no end-to-end result is claimed, and these C++ changes plus three new refusal/serialization cases remain uncompiled and unexecuted. Tests use isolated unsigned synthetic rows to verify refusal, not as proof of key ownership.


## Canonical index integration preparation (not compiled)

The future runtime source captures both the authenticated target historical inventory and the previous live index inventory. Rechecks compare the previous map independently. Index receipt domains now bind a separately tagged historical script/address map; empty-map receipts preserve their prior digest. Historical ownership overlapping a path registration is refused as ambiguous rather than silently relabeled.

Origin and full coverage reconcile existing row kind/path/reference against authenticated source ownership, persist historical coins with empty path and their exact recorded address, and write the checked source receipt. The target live map is allocated before writes, then swapped only after the index commit and its ordinary-wallet completion callback succeed. An ordinary commit refusal leaves the durable index prefix for retry while retaining the prior live authority. Wallet-bound index read/apply wrappers reauthenticate present imports in a caller-owned wallet snapshot before touching the index; borrowed transactions remain untouched.

Recovery inventory now accepts an exact authenticated historical owner in place of an invented address/path companion. Existing HD, modern import, PQ and reverse enumeration checks remain. The untyped legacy script/scan APIs explicitly refuse historical ownership; canonical delivery is required. This is a supported-path distinction, not proof that every old scanner caller has been integrated. Current source/private ownership is still scoped to present rows and captured sessions; deletion, backup rollback and complete cross-store readiness remain separate requirements.

Three additional C++ cases exercise SQL-free publication observation under ordinary commit refusal/retry, actual wallet-bound index authentication and borrowed transaction preservation, and a corrupted owner changed after capture refusing before index mutation. They use real historical key fixtures and canonical coverage; no fixture setter can publish ownership. All eight historical cases and the thirteen-file packet remain UNCOMPILED/UNEXECUTED. No original RED or completed discovery/reorg/release claim.

Rechecks also accept the already-published target inventory for an idempotent repeat, with existing source head, receipt prefix and row reconciliation still mandatory. An unrelated live inventory is refused. The publication test repeats the same source projection after success and requires both stores unchanged.


## Reorg and origin-row follow-up preparation (not compiled)

Origin reconciliation now checks the SQL type and explicit byte length of the transaction ID and script, requires canonical lowercase 64-digit hex, bounds the output index before conversion, and avoids null-terminated ID parsing or TEXT-to-BLOB coercion. Existing source coin/value/spend/typed-owner comparisons remain.

A ninth historical test uses the real historical import, actual funding/discovery and real Orchard proof/signature, mines the shield at104, checks typed input/change ownership and shield history, disconnects through the existing canonical fixture, reopens, then mines the identical envelope again. It requires preserved imported ciphertext/address/label, empty historical paths, exact retained transaction bytes, unchanged local history metadata, restored unspent input/removal of disconnected change, and receiver balance0/20000 across undo/reconnect. No historical coin or account is fabricated by the new test. All prior eight test bodies and the180-second registration remain unchanged. This new test and origin-row edit are UNCOMPILED/UNEXECUTED; no reorg qualification claim is made.
