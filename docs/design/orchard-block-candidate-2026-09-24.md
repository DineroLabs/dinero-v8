# Orchard mixed-block candidate and authorization coverage

## Implemented boundary

`OrchardBlockCandidate` owns the exact block bytes, header, ordered typed
transactions and Utreexo suffix. It cannot convert to the historical `Block`.
The historical decoder and production admission paths remain unchanged.

The staged decoder bounds allocation before reading transactions, requires a
canonical count and an explicit suffix flag, and rejects trailing bytes. A
present Utreexo suffix must round-trip through the shared proof codec exactly.
That canonical suffix requirement is a draft new-format rule, not a retroactive
restriction on historical replay. Full weight, proof-target and block-resource
validation still belong to block admission.

Transaction roots use the existing Merkle algorithm, including mutation
detection. The witness root preserves the zero coinbase leaf. Recognized DINW
commitments always validate; the selected-height caller controls whether a
witness-bearing block must include one. These identity checks do not validate
transaction signatures, PoW or Utreexo proofs.

## State staging

The ChainDB adapter requires this complete candidate. Its header identity must
match the selected block context. Every Orchard transaction must match the next
sealed authorization byte for byte, including transparent witness bytes. Missing,
extra, reordered or substituted authorizations cannot stage a partial update.
Duplicate transaction identities, additional coinbases and retired legacy
shielded transactions are rejected before any Orchard writes are staged.

Ordinary transaction authorization, ordered cross-family coin spending, total
fees and the coinbase bound are still obligations of the mixed-block connector.
An Orchard state preparation is not a full block-validity certificate.

## Qualification

Component tests cover bounded/truncated framing, suffix canonicality, independent
two-leaf roots, shared historical roots, DINW presence and mismatches, and exact
candidate/authorization coverage. The fixtures are synthetic candidate blocks,
not mined blocks or a production-node lifecycle rehearsal.

The reader passed ASan/UBSan with all 19 project C++ translation units selected
by its link map instrumented consistently. Rust and external C dependencies were
not instrumented; macOS leak detection was disabled. Earlier partial-instrumentation
runs produced libc++ container-annotation failures, retained in private evidence.
No sanitizer detector was disabled to obtain the passing reader result.
