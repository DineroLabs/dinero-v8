# Orchard wallet proof construction

This is an optional component on the v8.1.13 integration branch. It does not
create a live wallet RPC or activate an Orchard transaction type.

## Shield construction

`WalletBundlePlan::PrepareShield` takes an opaque ZIP32 sender-key owner and one to
eight typed receiver/amount/memo payments. Upstream Orchard creates the padded
bundle using OS randomness. The plan owns its unproved actions and exposes only
the effects needed for the transaction signing context. It is move-only.

`Prove(context)` consumes the plan. The C++ host derives the signing message
privately from that plan's effects and its owned transaction context; there is
no public caller-digest overload. A mismatched transparent funding balance fails
before proof work. Actual input provenance and transparent signatures remain the
responsibility of the authenticated coin/scripting path.

Proof construction uses the pinned Orchard v2 circuit, a lazily initialized
proving key, a dedicated two-worker pool and one simultaneous proof. This is not
a bounded/cancellable wallet-service queue yet. The Rust boundary encodes the
result canonically, decodes it again, compares the planned effects and verifies
the authorization and proof before returning bytes. C++ independently decodes,
compares the planned fields and verifies against its context before returning
`ProvedWalletBundle`. No private key is exported through the C ABI.

## Tests and limits

The Rust construction test decrypts a fresh cross-address output and compares
recipient, amount and memo. Two independently prepared plans have different
effects. Bounds, ABI layout and unchanged error outputs are checked. The C++
`OrchardWalletShield` test makes a fresh proof, checks its round trip, rejects a
funding mismatch before proving, rejects reuse, and requires verification to
fail under a different transaction input or network. These are synthetic keys
and transactions, not a real wallet operation.

## Receive, send and unshield

`WalletNote::Receive` requires a sealed `VerifiedAuthorization`, a viewing key,
scope and action index. It uses upstream authenticated note decryption and
returns an opaque owned note or no match. Neither receipt nor proof validity
establishes selected-chain inclusion, confirmation or unspentness.

`WalletWitness` uses the pinned upstream incremental witness implementation.
Creating a witness takes the parent frontier and all ordered commitments in the
new bundle, including padding. Append creates a new witness, checks both expected
roots and preserves the old witness for rollback. `PrepareSpend` checks account
ownership, duplicate notes, bounds and each note's membership at the common
anchor before preparing a plan. An empty payment list is an unshield; the same
owned-context proof/signing path checks the resulting transparent withdrawal.
The plan's copied spending-key bytes are zeroized on drop. This does not claim
that all temporary upstream note/key objects or compiler copies are wiped.

`OrchardWalletSpend` generates three fresh proofs through these APIs: shield to
wallet B, receive and update its witness, send to wallet C, then unshield. It
checks exact amounts, nullifiers, envelope binding and the total withdrawal plus
fees. Wrong account, duplicate note and wrong anchor are rejected. A separate
incremental test follows four witnesses across 256 additional commitments,
checking roots, paths, counts and immutable parents.

Protected wallet storage, durable witnesses, authenticated chain scanning,
operation recovery, transparent signing and runtime admission remain separate
implementation work. Returning a proved bundle does
not establish that its transaction is currently admissible or mined.
