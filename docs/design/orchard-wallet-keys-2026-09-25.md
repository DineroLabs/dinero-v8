# Orchard wallet key and receiver profile (draft)

This component implements the first wallet primitives; it does not yet connect the wallet database, RPCs, proving, scanning, witness maintenance or send operations. It reads no existing wallet and changes no live data.

## Key derivation

Use the pinned upstream Orchard ZIP32 implementation at `m/32'/1448'/account'`. The coin type is Dinero's existing 1448, checked across the host constant and Rust ABI. The purpose is Orchard ZIP32's 32, **not** the retired protocol's purpose 77. No legacy key or note is reinterpreted. Accounts are below 2^31; seeds are 32..252 bytes, matching the pinned upstream bounds. The caller supplies its protected master seed and is responsible for entropy, seed lifetime, backup and unlock policy.

An opaque, move-only C++ owner holds a non-cloneable Rust key handle. There is no raw spending-key export or logging interface. Stored spending-key bytes are wiped on drop using pinned `zeroize`. This does not establish secure erasure of upstream temporary key objects, compiler copies, the caller's seed, swap or process dumps. Those limits and the host wallet's secure-memory handling remain part of integration review. All FFI calls are panic-contained; destruction consumes the handle and C++ terminates on a failed destructor boundary.

Full viewing keys are 96-byte upstream encodings and are privacy-sensitive, even though they cannot spend. Watch-only derivation uses the same validated FVK. Scope 0 is external receive and scope 1 is internal change; all other values fail. The diversifier index is the full 88-bit little-endian ZIP32 index. A wallet must persist its next issued index atomically and must not reset it on restore; that database work is still pending.

## Human-readable receiver

The draft Dinero Orchard address uses Bech32m with payload byte `01` followed by the canonical 43-byte upstream Orchard receiver. HRPs are `dinorch` (mainnet), `tdinorch` (testnet), and `rdinorch` (regtest). This avoids confusing the new pool with retired `dins` addresses or transparent addresses. These names are a proposed wallet profile, not an activated protocol.

Decode requires the expected network explicitly, a valid Bech32m checksum, exactly 44 payload bytes, profile 1 and an upstream-valid non-identity receiver. It requires exact re-encoding, which rejects excess symbols/nonzero padding. Consistent uppercase is accepted as a display variant; mixed case is rejected. Generated text is lowercase, bounded to 90 characters. No expected-network default or automatic network acceptance exists. The same ZIP32 key material can derive receivers on development networks, while HRPs prevent accidentally accepting their address text as mainnet. The transaction signing domain separately binds network/genesis/branch.

Address validity cannot prove that the intended recipient controls the address; the pinned upstream receiver parser makes that same distinction. It must not become a claim of ownership or recoverability in a wallet UI.

## Verification

Rust and C++ tests cover literal-path parity against upstream, account/scope/full-index separation, deterministic seed restore, watch-only parity, move-only ownership, invalid seeds/accounts/scopes/networks/FVKs, checked receiver encoding, checksum/profile/case rules and unchanged FFI outputs on failure. Dependencies are exact existing lockfile versions; no dependency was upgraded.

Wallet proof construction, durable notes/operations, full-node admission, loaded-node and platform qualification remain outstanding. Mainnet activation stays unset.
