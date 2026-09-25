# Orchard wallet proof construction

This is an optional component on the v8.1.13 integration branch. It does not
create a live wallet RPC or activate an Orchard transaction type.

## Shield construction

`WalletShieldPlan::Prepare` takes an opaque ZIP32 sender-key owner and one to
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

Spending, unshielding, chain-authenticated note discovery, witness maintenance,
protected wallet storage, operation recovery, transparent signing and runtime
admission remain separate implementation work. Returning a proved bundle does
not establish that its transaction is currently admissible or mined.
