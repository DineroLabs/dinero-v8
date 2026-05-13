# Dinero v7 Wallet Schema & PQ Key Derivation

**Status:** Design doc (pre-implementation).
**Scope:** How v7 wallets derive, store, and handle ML-DSA-65 keys.
**Relation to other docs:**
- `V7_GENESIS_SPEC.md` — consensus surface (unchanged by this doc).
- `V7_PQ_LIBRARY_SELECTION.md` — why PQClean.
- `include/consensus/pq/ml_dsa_65.h` — the `KeygenFromSeed(seed)` primitive.
- `include/consensus/pq/test_vectors/` — cross-arch reproducibility anchors.

This doc answers three questions:

1. How does a wallet seed (BIP-39 mnemonic → BIP-32 extended key) turn into an ML-DSA-65 keypair?
2. Where and how are the ~4 KB ML-DSA secret keys persisted?
3. How do wallets handle the per-leaf PQ keys that sit under a P2MR Merkle root?

## 1. Derivation Path

### Input

- **Wallet seed (16–64 bytes)** — BIP-39 mnemonic → PBKDF2-HMAC-SHA512 → master seed, same as current Dinero and Bitcoin wallets.
- **Derivation path** — `m/88' / <coin_type> / <account> / <change> / <address_index> / <leaf_index>`.

### Key derivation algorithm

BIP-32 is designed for secp256k1 (scalar keys). It does **not** extend cleanly to ML-DSA: the secret key is 4032 bytes and has internal structure (ρ, ρ', K, tr, s1, s2, t0), not a scalar. We therefore use BIP-32 **only** as a key-derivation path to produce a 32-byte seed per address, then feed that seed into the explicit `KeygenFromSeed` primitive.

```
bip32_derive(master_seed, "m/88'/1448'/account'/change/address_index") -> extended_key
pq_seed = HKDF-SHA256(
    ikm  = extended_key.private_key || extended_key.chain_code,
    salt = "dinero-v7-ml-dsa-65",
    info = LE32(leaf_index),
    L    = 32
)
keypair = KeygenFromSeed(pq_seed)
```

All fields above are **consensus-portability-critical** — once a wallet exists in the wild with these values, changing any of them invalidates keys already derived. They are locked:

- `coin_type = 1448` (see Coin type section below).
- HKDF construction: HKDF-SHA256 (RFC 5869), length 32 bytes.
- HKDF `salt` = the ASCII bytes `dinero-v7-ml-dsa-65` (19 bytes, no null terminator, no trailing newline, case-sensitive).
- HKDF `ikm` = the 32-byte BIP-32 private-key bytes concatenated with the 32-byte chain_code bytes, in that order. Total 64 bytes.
- HKDF `info` = the 4-byte little-endian encoding of `leaf_index`. For single-leaf P2MR (the v7-genesis default), `leaf_index = 0` and `info` is four zero bytes.
- `KeygenFromSeed` is the explicit-seeded path in `src/consensus/pq/ml_dsa_65_keygen.c`. Cross-arch reproducibility is anchored by `include/consensus/pq/test_vectors/ml_dsa_65_keygen_vectors.h`.

Rationale:

- **BIP-32 is reused verbatim** for the path-to-byte-material derivation. Every existing BIP-39 / BIP-32 wallet library works unchanged for the path walk.
- **HKDF-SHA256 is the bridge.** It's scheme-agnostic, widely implemented, and cryptographically sound for converting non-uniform BIP-32 output into a uniform 32-byte seed. The salt string domain-separates v7 PQ keys from any other use of the same BIP-32 extended key.
- **`leaf_index` appears in HKDF's `info` field** so a single P2MR Merkle root can cover multiple ML-DSA keys under one BIP-32 address. See §3.
- **`KeygenFromSeed` is the only place PQ-specific logic lives.** Reproducible cross-architecture (anchored by `include/consensus/pq/test_vectors/`).

### Coin type

`coin_type` is the BIP-44 SLIP-44 registered number.

- **v5: `1447`** (`include/dinero/core/consensus/coin_type.h:21` — `DINERO_COIN_TYPE = 1447`). The public SLIP-44 registry lists `1447` as `DNR | Dinero`. This stays unchanged.
- **v7: `1448`** — **locked**. Ticker `DIN` (not `DNR` — v7 is a fresh-genesis chain with its own identity). SLIP-44 registration filed at [satoshilabs/slips#2005](https://github.com/satoshilabs/slips/pull/2005):
  ```
  1448 | 0x800005a8 | DIN | Dinero v7
  ```

The `DIN` ticker choice is deliberate:

- v5 continues to use `DNR`.
- v7 uses `DIN`, both because the fresh-genesis chain deserves its own brand and because `Dinero → DIN` is the natural short form.
- There is a legacy SLIP-44 entry `447 | DIN | Dinero` predating `1447`. It is unused today. v7 does **not** claim `447` — it claims `1448`. If the community wishes to retire the legacy `447` entry, that's an independent cleanup; it does not affect v7 derivation.

**Status caveat.** Until PR #2005 merges, treat `1448` as a Dinero-internal allocation — correct and locked, but not yet externally authoritative. Third-party wallets implementing v7 support should note this in their own docs and update once the registry entry lands.

**Why v7 changes coin_type.** v7 is a fresh-genesis chain, not a v5 upgrade. The wallet identity model is fundamentally different (ML-DSA-65 instead of secp256k1 Schnorr). A distinct coin_type gives hard separation for:

- shared-mnemonic users (one mnemonic, two chains, no key reuse)
- wallet discovery and scanning (scan only the right subtree per chain)
- export/import tooling (dumpwallet tags make the source chain explicit)
- hardware-wallet path assumptions (a Ledger v5 app vs. hypothetical v7 app expect different `m/44'/X'` subtrees)

A wallet walking a shared mnemonic computes:
```
v5 xprv = BIP32(seed, m/44'/1447'/...)
v7 xprv = BIP32(seed, m/88'/1448'/...)
```
No shared private-key material between chains. This matters for the v5→v7 claim path: a v5 Schnorr signature proving ownership of a v5 UTXO is signed by the v5-derived key, and authorizes v7 value to the v7-derived P2MR address. They never collide.

### Hardened derivation

- `m/88'`, `coin_type'` (1448'), and `account'` are **hardened** (marked with `'`).
- `change`, `address_index`, and `leaf_index` are **non-hardened**, matching BIP-44 convention. Non-hardening these components lets watch-only wallets derive pubkeys without secret material, and preserves the usual gap-limit scan behavior.

**Locked.**

## 2. Wallet Storage Schema

### What to store

Per PQ address:

| Field | Size | Notes |
|---|---:|---|
| `seed` | 32 B | The output of the HKDF above |
| `pubkey` | 1952 B | Derived from seed, stored for fast lookup |
| `merkle_root` | 32 B | The P2MR commitment (address bytes) |
| `address_string` | ~63 B | bech32m-encoded `din1r...` |
| `derivation_path` | ~50 B | BIP-32 path string |
| `leaf_index` | 4 B | If using multi-leaf Merkle trees |
| `label`, `created_at`, etc. | — | Wallet metadata |

**The secret key bytes are NOT stored.** The wallet re-derives from `seed` via `KeygenFromSeed` on demand. Benefits:

- Storage per-address drops from 4 KB + 2 KB (secret + pubkey) to 32 B + 2 KB.
- Encryption surface is 32 B/address instead of 4 KB/address.
- Wallet backup size is dominated by pubkeys, which are public anyway.
- If the pubkey ever disagrees with `KeygenFromSeed(seed)`, we have a corruption signal.

### Encryption at rest

Dinero v5's existing wallet encryption stack uses:

- Argon2id for KDF (master password → wallet encryption key).
- AES-256-GCM for per-field encryption.

Applied to v7 PQ keys:

- **Seed field is encrypted** (32 B plaintext → ~48 B ciphertext with GCM tag).
- **Pubkey, merkle_root, derivation_path, label are NOT encrypted.** They're public.
- **Address string is NOT encrypted.** Already on-chain every time the address is used.

If the wallet is locked, the pubkey is still accessible (so the UI can display addresses, balances, history). Signing requires unlocking to decrypt the seed, run `KeygenFromSeed`, produce the signature, and zeroize the secret.

### Sqlite schema (proposed)

Extends the existing wallet DB (`wallet_registry.db` or equivalent) with a new table:

```sql
CREATE TABLE v7_p2mr_addresses (
    id               INTEGER PRIMARY KEY,
    wallet_id        INTEGER NOT NULL,
    address          TEXT NOT NULL UNIQUE,           -- "din1r..."
    merkle_root      BLOB NOT NULL,                  -- 32 bytes
    pubkey           BLOB NOT NULL,                  -- 1952 bytes
    seed_encrypted   BLOB NOT NULL,                  -- AES-256-GCM of 32-byte seed
    seed_nonce       BLOB NOT NULL,                  -- 12-byte GCM nonce
    seed_tag         BLOB NOT NULL,                  -- 16-byte GCM tag
    derivation_path  TEXT NOT NULL,
    leaf_index       INTEGER NOT NULL DEFAULT 0,
    label            TEXT,
    created_at       INTEGER NOT NULL,
    UNIQUE(wallet_id, derivation_path, leaf_index)
);
CREATE INDEX idx_v7_p2mr_addresses_wallet ON v7_p2mr_addresses(wallet_id);
CREATE INDEX idx_v7_p2mr_addresses_merkle_root ON v7_p2mr_addresses(merkle_root);
```

The secret key is never stored. Derivation from the encrypted seed happens in RAM and is zeroized after use.

### Zeroization

- **Primitive: `OPENSSL_cleanse`.** Reuses v5's existing zeroization convention (`src/wallet/bip32_deriver.cpp:79,81,94,96`; `src/crypto/hd_keychain.cpp:38-39`). No new third-party dependency; OpenSSL 3.3.2 is already vendored.
- `Keypair` returned by `KeygenFromSeed` holds the 4032-byte secret on the heap. After signing, the wallet MUST overwrite the secret bytes before drop. The C++ façade provides a `SecureKeypair` RAII wrapper around `ml_dsa_65::Keypair` that calls `OPENSSL_cleanse` on its `secret` array in its destructor.
- The 32-byte PQ seed is zeroized after `KeygenFromSeed` returns, before the seed leaves local scope.
- The 64-byte HKDF `ikm` buffer (BIP-32 private key || chain_code) is zeroized immediately after HKDF returns.

**Locked.**

## 3. Multi-Leaf Merkle Roots (Future Extension)

v7 at genesis uses the simplest case: **Merkle root = SHA256(`scheme_id=0x01` || `ml_dsa_pubkey`)**, a single leaf under the root. The root is what the address commits to; the single leaf is the single ML-DSA key that can spend.

Post-genesis, the same Merkle layout supports multiple keys per address:

- Single-leaf P2MR (v7 default): 1 key per address, simplest.
- Multi-leaf P2MR (future opt-in): 2–256 keys under one root, one address with multiple independent spend paths. Natural for:
  - Rotation: key A for year 1, key B for year 2 committed at receive time, spender picks leaf + reveals path.
  - Multi-scheme: once FALCON activates, a leaf with an ML-DSA key and a leaf with a FALCON key under one address.
  - N-of-M-like policy via Merkle + predicate (much further out, script-level).

The wallet schema already supports this via `leaf_index` in the derivation path. Keys `m/.../index/0`, `m/.../index/1`, …, `m/.../index/k-1` are all committed under one Merkle root at address derivation time. The wallet records `leaf_index` per key.

At v7 genesis we pin to single-leaf to keep the wallet UX simple. The schema doesn't change when multi-leaf ships.

## 4. RPC Surface (Phase 4c preview)

These are what the wallet RPCs will eventually expose. Not implemented yet; this section is scope for Phase 4c.

```
wallet.getnewp2mraddress
  params: { account?: int, label?: string }
  returns: { address: "din1r...", merkle_root_hex: "...", derivation_path: "m/44'/..." }

wallet.listp2mraddresses
  params: { wallet_id?: int, count?: int, offset?: int }
  returns: [{ address, merkle_root_hex, pubkey_hex, derivation_path, label, created_at }, ...]

wallet.signp2mr
  params: { address: "din1r...", sighash_hex: "..." }   # sighash is BIP341-style
  returns: { scheme_id: 1, pubkey_hex: "...", signature_hex: "...", merkle_path_hex: "..." }
  preconditions: wallet unlocked; address exists in wallet

wallet.exportp2mrseed
  params: { address: "din1r...", passphrase: "..." }
  returns: { seed_hex: "..." }
  preconditions: wallet unlocked; user confirms via passphrase re-entry
  # For cold-wallet migration only. Never exposed to wire RPC.

wallet.importp2mrseed
  params: { seed_hex: "...", label?: string, account?: int }
  returns: { address: "din1r...", merkle_root_hex: "..." }
  # Accepts the 32-byte seed directly, re-derives pubkey, stores.
```

## 5. Non-Goals

- **Shielded / confidential amounts.** v7 is transparent-only. See `V7_GENESIS_SPEC.md`.
- **Silent Payments over P2MR.** Requires a PQ stealth scheme (research-level). Out of scope.
- **Deterministic signing.** PQClean signing is hedged by default; matches NIST FIPS 204 guidance. Wallet exposes only non-deterministic sign at genesis.
- **Hardware wallet integration.** Signal path + USB protocol extensions come after RPC + Qt integration lands. Out of scope for this doc.
- **Multi-sig / threshold ML-DSA.** Not a v7 concern.

## 5b. Daemon integration — master key sourcing

v7 handlers need a 32-byte AEAD master key to seal / open per-address
pq_seeds (§2). That key MUST be derivable from the user's unlocked
wallet session, but MUST NOT travel over the JSON-RPC wire.

**Locked decision: Option B — persisted per-wallet v7 master key,
encrypted under the same PBKDF2 key that protects v5's master seed.**

Rationale:

- Independent lifetime from v5's master seed. Rekeying v5 (password
  change) re-encrypts the v7 master key alongside v5's seed; v7
  ciphertexts in the `v7_p2mr_addresses` table stay valid because the
  master key inside is unchanged.
- No cross-scheme rekey churn — rotating the v5 password does NOT
  invalidate already-sealed v7 `seed_ciphertext` blobs.
- Clean audit: the v7 master key is a persisted artifact in one row,
  with a known SHA-256 derivation path to every per-address seal.
- Matches v5's existing convention — `hd_seeds` already stores an
  encrypted master_seed blob with a random salt and AES-256-GCM;
  v7's master key slots in as a sibling column.

### Storage (zero schema migration)

The v7 PQ master key is persisted as a **single wallet-settings row**,
not a new column on `hd_seeds`. The existing settings key-value store
(`wallet_meta` — used by v5 for `wallet_salt`, `wallet_verify_hash`,
etc.) accepts hex-encoded ciphertext without any DDL change:

- Setting key: `v7_pq_master_key_encrypted`
- Setting value: hex-encoded output of `encryptData(plaintext_32B,
  encryption_key_)`, which v5 already defines as
  `nonce(12) || ciphertext(32) || gcm_tag(16)` → 60 bytes → 120 hex chars.

Pre-existing wallets (no setting row yet) upgrade lazily on first
unlock: `WalletManager::unlockWallet` detects the missing row,
generates a random 32-byte v7 master key, calls `encryptData` using
the PBKDF2-derived `encryption_key_`, and persists via `setSetting`.
No ALTER TABLE, no schema version bump. The upgrade is idempotent and
restartable — if the wallet crashes mid-unlock before the setting is
written, the next unlock regenerates with the same encryption key
material available.

Trade against the "new column on `hd_seeds`" option considered earlier:

- Settings-row: zero DDL, matches v5's existing conventions for
  encrypted single-value blobs (e.g., `wallet_verify_hash`), less
  risky surface on a live v5 wallet.
- `hd_seeds` column: slightly more "structured" but requires ALTER
  TABLE on every existing wallet DB; no operational benefit for a
  single 32-byte value.

Settings-row is the chosen implementation.

### In-memory cache

`WalletManager` gains one private field alongside the existing
`encryption_key_` / `master_seed_`:

```cpp
std::array<uint8_t, 32> pq_master_key_{};   // zeroized on lock
```

`lockWallet` zeroizes it via `OPENSSL_cleanse`, identical to how it
handles `encryption_key_` and `master_seed_`.

### Public accessor

RPC handlers receive the master key via a narrow WalletManager method:

```cpp
// Returns the cached 32-byte v7 PQ master key when the wallet is
// unlocked. Returns nullopt if locked. Never exposes the raw bytes
// over JSON-RPC — handlers copy into an AeadKey, use, scrub.
std::optional<std::array<uint8_t, 32>> GetV7PqMasterKey() const;
```

The key is never serialized into any RPC response body. The only
callers are in-process handlers.

### BIP-32 material

A sibling method walks the v7-specific BIP-32 path and returns the
32-byte private key + 32-byte chain code needed to feed
`wallet::pq::DerivePQKeypair`:

```cpp
struct V7Bip32Material {
    std::array<uint8_t, 32> private_key;
    std::array<uint8_t, 32> chain_code;
};

// Walks m/88'/1448'/account'/change/address_index against the
// wallet's master_seed_ using the existing BIP32Deriver engine.
// Returns nullopt if the wallet is locked.
std::optional<V7Bip32Material> DeriveV7Bip32Material(
    uint32_t account, uint32_t change, uint32_t address_index) const;
```

Both values are scrubbed from any intermediate buffers before this
method returns; the returned struct is a caller-owned copy that the
RPC handler promptly forwards into `DerivePQKeypair` (which takes
them by value and scrubs again, §4c.1).

## 6. Import / export

### Index backfill on import

**Locked: 20 indices per `change` branch.** When a wallet imports a seed (either via mnemonic or via `wallet.importp2mrseed`), it scans `leaf_index` from 0 upward, deriving the pubkey at each step, checking for on-chain activity (received funds, past spends). It continues until it encounters 20 consecutive unused indices. This is the BIP-44 gap-limit convention.

**Tradeoff:** a user who manually generates more than 20 consecutive addresses without using any of them loses those addresses on import. Qt UI should discourage this by generating addresses lazily (only when the user asks for one); RPC users are on their own. Gap limit of 20 has been the Bitcoin/Ethereum/Litecoin convention for a decade and is understood by downstream tools.

### Wallet export format

**Locked: extend the existing v5 `wallet.dumpwallet` line-based format.** v5 exports one private key per line, tagged with optional label and derivation path. v7 extends this with a new tagged line type:

```
# Dinero wallet dump
# Generated: 2026-04-16 14:32:05 UTC
# --- v5 transparent keys ---
K<wif-private-key> 2026-04-01 label=main derivation="m/44'/1447'/0'/0/0"
...
# --- v7 P2MR keys ---
pq-v7-mldsa65 <bech32-encoded-32-byte-seed> 2026-04-16 label=pq-main derivation="m/88'/1448'/0'/0/0" leaf=0
...
```

Rationale:

- **Line-based plaintext**, matching v5 convention. User is already responsible for securing the dump file.
- **One tagged line type per v7 PQ key.** The tag `pq-v7-mldsa65` carries both the version (`v7`) and the scheme (`mldsa65`), so future FALCON activation adds `pq-v7-falcon512` lines without breaking parsers that only know ML-DSA.
- **The 32-byte seed is exported, not the 4032-byte secret.** Smaller, faster import, and the import path runs `KeygenFromSeed` to reconstruct the secret deterministically. Matches the storage-schema decision from §2.
- **Bech32 encoding of the seed** (not raw hex) for the same reason Bitcoin uses WIF for private keys: built-in typo detection via the bech32 checksum. HRP: `dinpq1v7mldsa65`. Encoding convention pinned when the import/export code lands.
- A parser that only knows v5 format ignores unknown `pq-v7-*` tag lines gracefully. Round-trip backward compat preserved.

**Encrypted-backup path (`wallet.notarizebackup`)** is stubbed in v5 and out of scope for the v7 wallet schema. If/when it's implemented properly, it wraps the plaintext dump with the existing Argon2id + AES-256-GCM stack.

## 7. Open Items

All Phase 4b' wallet-identity open items are resolved (see sections 1–6 above). The only remaining open items for Phase 4c implementation work are **not portability-breaking** — they can land during RPC implementation without invalidating existing vectors:

- **Exact encoded-seed HRP for dumpwallet** (`dinpq1v7mldsa65` vs. alternative). Bikesheddable; lock at implementation time.
- **Qt address-book UX** for the `leaf_index` field (hidden by default? expert-only?). Product decision, post-RPC.
- **Hardware wallet integration path.** Trezor / Ledger ML-DSA support is upstream-dependent. Out of scope until upstream firmware support lands.

All other consensus-portability-critical decisions are now locked. Phase 4c (RPC implementation) can proceed against a stable schema.
