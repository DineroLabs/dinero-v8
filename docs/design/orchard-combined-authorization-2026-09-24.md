# Combined authorization component

`VerifyOrchardAuthorizations` checks transparent signatures, maturity and locks,
then Orchard proof and signatures against the same owned coin snapshot and
transaction. Its sealed result compares intent, canonical bytes and both
transaction identities across the two paths. No raw digest or authorization
cache verdict can be substituted at this C++ boundary.

This is not admission. A runtime caller still needs an authenticated branch
view held consistently through validation and application, active anchors,
fresh nullifiers, pool conservation, activation, and atomic state/undo writes.
No live parser, mempool, relay, block or wallet path is enabled by this change.

## Honest fixtures

`make_combined_fixture.rs` uses unmodified pinned Orchard 0.15.5 to construct a
5,000-una shield and then spend the resulting note into a 4,500-una note for a
different address, withdrawing 500 una. It decrypts both recipient notes and
verifies each generated proof and all Orchard signatures before saving bytes.
The spend witness includes both emitted commitments, in their actual order.
All seeds and keys are public synthetic test data. No wallet, RPC or live chain
is read. Fixture generation does not claim wallet integration.

The transactions use distinct mock transparent inputs and real P2TR/P2WPKH
signatures, with two full native witness-program outputs and a 666-una fee.
These inputs are supplied by a test coin view, not mined funding transactions.
Rust constructs the complete outer signing preimage independently; Python
checks its framing/hash using the saved effect as opaque data, and C++ derives
the same digest from the resolved transaction. This is not an independent
implementation of the Orchard effect commitment or circuit.

`OrchardAuthorization` checks both honest cases, typed-wire round trips,
historical-parser rejection, and independent rejection of altered transparent
signatures, proof, spend signature, binding signature and ciphertext. It shows
that transparent-only success cannot substitute for Orchard verification and
vice versa. Immature transparent funding is refused before success is returned.

## Qualification scope

Native macOS Release passes all six standalone and seven selected root CTest
entries. The existing eleven Rust cases remain included. The generator builds
under locked dependencies and strict Clippy; independent Python digest checks
pass. The new test is mandatory in both Linux workflow inventories.

ASan/UBSan passes for this C++ combined test, authorization/transparent/coin
components, typed reader and backend wrapper/signing/envelope sources. The
linked Rust, secp256k1 and historical-codec archives are uninstrumented, and
macOS leak detection is disabled. This is not whole-daemon sanitizer coverage.

Full daemon admission, wallet behavior, persistent state, reorg/reindex/crash
recovery, Linux qualification of this source and external protocol review are
separate remaining gates. Mainnet activation stays unset.
