# Trustless Light-Client Shielded Scanning — Design Spec

**Status:** Draft
**Date:** 2026-05-26
**Scope:** DineroDPI iOS (Swift) + dinero-v8 daemon (C++)
**Primary engineering site:** DineroDPI (`/Users/haydarevich/src/apps/DineroDPI`)
**Secondary engineering site:** dinero-v8 (optional daemon-side filter + witness RPC)

## Goal

Let an iOS DineroDPI wallet running in **thin-client mode** (no embedded local dinerod, talking only to one or more remote dinerod RPC endpoints) **see, balance, and history-render** its shielded notes — without trusting any remote RPC, without leaking the wallet's view key, and with privacy properties at least as strong as iOS's existing trustless transparent UTXO discovery.

Spending a discovered note (constructing and broadcasting a `wallet.transfer` / `wallet.unshield` bundle) is **Phase 2** of this spec; Phase 1 is receive-only (scan + balance + history).

This closes the only remaining strategic gap in Dinero's privacy stack: the chain has the shielded pool live since block 8650 (2026-04-28); the Qt and iOS-Embedded-Node clients have full shielded UX; only the iOS-thin-client mode lacks a way to participate without surrendering privacy or relying on an embedded local node.

## Non-goals

- **Hosted Light Wallet Service (à la Zcash lightwalletd).** Existing Zcash light-wallet architecture leaks viewing-key shape to the indexer server; we explicitly avoid that model. iOS in thin-client mode talks to a normal dinerod via standard RPCs, not to a specialized indexer.
- **View key sharing.** The IVK / OVK / NK never leave the iOS device. There is no "outsourced view key" mode in this design.
- **PQ-shielded.** The current shielded pool is classical-private (secp256k1 + Spartan/Hyrax transparent setup). Composing PQ into shielded is a separate research project. This spec layers on top of the existing classical shielded substrate as-is.
- **Spend in Phase 1.** Phase 2 covers spend; receive-only is the M1 milestone.

## Trust model

iOS in thin-client mode talks to one or more remote dinerod nodes through the existing `RpcClient` + `FilterChainSync` + `FilterBasedDiscovery` infrastructure. The remote node:

1. **Can see** which blocks the client requests (`blockchain.getblock <hash>`). This is unchanged from the existing transparent light-client behavior.
2. **Cannot see** which shielded outputs in those blocks belong to the client. The client trial-decrypts every shielded output's `encrypted_note` locally and silently discards non-matches; the remote node has no signal of which (if any) succeeded.
3. **Cannot forge** balances or note ownership. Every claimed note must round-trip through a locally maintained incremental shielded commitment tree anchored at a PoW-checked block hash; every spend (Phase 2) generates its own Spartan proof client-side.
4. **Can withhold blocks** (DoS), in which case the client doesn't learn about new notes until a different RPC endpoint serves them. This is the same DoS surface as transparent light-client mode and is mitigated by the existing multi-endpoint fallback logic in `NodeKit.shared`.

Privacy property summary: an adversary who controls every RPC endpoint the client ever talks to learns the wallet's blocks-of-interest set but does **not** learn which outputs in those blocks the wallet owns, the wallet's value flow, or anything about the wallet's view key. This is materially better than Zcash light-client mode (server sees scan-request shape) and Monero light-client mode (server sees value via outsourced view-key).

## Architecture overview

Two layers, one prerequisite (shielded pool already live on mainnet — done).

**iOS layer (substantial):**

```
DineroDPI/Core/
  Crypto/
    Poseidon.swift         (new) — Poseidon-2 over secp256k1 scalar field
    HashToCurve.swift      (new) — XMD:SHA-256_SSWU_RO_ per RFC 9380
    ChaCha20Poly1305.swift (new) — RFC 8439 AEAD (separate from vault GCM)
    HKDF.swift             (verify or new)
  Shielded/
    ShieldedKeys.swift     (new) — sk → ask/nsk → ak/nk → ovk/ivk/dk → pk_d derivation
    ShieldedAddress.swift  (new) — Bech32m encode/decode of din1s… / tdins… / rdins…
    NoteCipher.swift       (new) — encrypted_note layout + trial-decrypt
    CommitmentTree.swift   (new) — Sapling-shape Merkle frontier + membership verify
    AnchorVerifier.swift   (new) — pin verification to PoW-anchored block hash
    ShieldedNote.swift     (new) — value-type for discovered + verified notes
  Filters/
    FilterBasedDiscovery.swift     (modify) — also enumerate shielded outputs
    ShieldedNoteDiscovery.swift    (new) — orchestrator analog of FilterBasedDiscovery
  Stores/
    ShieldedNoteStore.swift        (new) — local persistence of discovered notes
```

**Daemon layer (small, optional but high-value):**

```
include/consensus/
  shielded_compact_filter.h (new) — GCS filter over recipient-diversifier tags

src/consensus/
  shielded_compact_filter.cpp (new)

src/rpc/
  shielded_lightclient_rpc.cpp (new) — three RPCs:
    blockchain.shielded.filter <hash>            -- returns per-block diversifier filter
    blockchain.shielded.outputs <hash>           -- returns just the shielded outputs of a block (cheaper than getblock)
    shielded.witness.by_index <leaf_idx> <anchor_height>  -- returns Merkle path (Phase 2)
```

The daemon-side filter is structurally identical to DNRF (`filter_commitment.h`) but indexes recipient-diversifier tags from shielded outputs instead of transparent scriptPubKey hashes. Without it, iOS must trial-decrypt every shielded output in every block-of-interest; with it, iOS skips blocks with no candidate diversifiers and trial-decrypts ~100× less.

## iOS detail

### Phase 1 (receive-only)

#### Crypto primitive ports

Every primitive listed below must match the daemon's reference implementation byte-for-byte. Test vectors live at `docs/specs/shielded_v030_test_vectors.md` and must round-trip on iOS.

- **Poseidon-2 over secp256k1 scalar field.** Port from `src/consensus/shielded/poseidon.cpp` (or wherever the daemon's Poseidon-2 lives — verify exact file before implementing). Hardest of the four ports because Poseidon is parameter-sensitive: round constants, S-box exponent, and MDS matrix all matter. The Swift implementation MUST use the same parameter set the daemon uses. **Performance unknown.** All per-block timing estimates in this spec assume Swift Poseidon-2 reaches throughput close to the daemon's C++ implementation; realistic ranges without SIMD intrinsics or hand-tuning are 3-5× slower, which would push first-sync from ~32 s to ~60-110 s. If the day-2 micro-benchmark misses budget, the mitigation is to expose the daemon's Poseidon as a `.xcframework` (precompiled C++) — the same technique M3 plans for the Spartan prover. Don't ship a slow pure-Swift Poseidon if it bottlenecks the scanner.
- **Hash-to-curve XMD:SHA-256_SSWU_RO_ per RFC 9380 §8.7.** Used for deriving `pk_d = hash_to_point(d) · ivk`. CryptoKit lacks this; pure-Swift implementation following the RFC.
- **ChaCha20-Poly1305 IETF (RFC 8439), 12-byte nonce.** CryptoKit has `ChaChaPoly` natively (`CryptoKit.ChaChaPoly`), and the daemon path in `src/wallet/shielded_derivation.cpp` uses the same IETF profile: nonce = 12 zero bytes, AAD = the 32-byte `epk`, output = ciphertext || 16-byte tag. M1 day-1 spike: verify CryptoKit's `SealedBox` combined/detached layout against this daemon byte layout. If it does not match cleanly, fall back to a pure-Swift port (~100 LOC).
- **HKDF-SHA256 (RFC 5869).** CryptoKit has `HKDF<SHA256>`. The daemon's `kAeadInfo` is the ASCII bytes `DIN/v7/shielded/note`, with salt = `epk[0..32]` and IKM = the 32-byte ECDH shared secret. M1 day-1 spike: verify CryptoKit's `info`/`salt` semantics produce the same key bytes.

#### Shielded key derivation

Per `docs/specs/shielded_derivation.md`:

```
BIP32 path: m/99' / 1448' / account'

sk (32 bytes) = derived child key
ask = sk[0..32] reduced mod q          // spend authorization key
nsk = sk[32..64] reduced mod q          // nullifier private key
ovk = HKDF(sk, "DIN/v7/sh/ovk/v1") [0..32]  // outgoing view key
dk  = HKDF(sk, "DIN/v7/sh/dk/v1")  [0..32]  // diversifier key

ak   = ask · G  (BIP340-canonical)      // spend pubkey
nk   = nsk · H  (BIP340-canonical)      // nullifier pubkey
ivk  = blake2s_personal(ak || nk, "DIN/v7/sh/ivk")[0..32] reduced mod q

d   = diversifier (11 bytes), enumerated via dk
pk_d = hash_to_curve(d) · ivk           // diversified public key
```

Address encoding (Bech32m HRP):
- mainnet: `dins`
- testnet: `tdins`
- regtest: `rdins`

Address payload: `d (11 bytes) || pk_d (32 bytes BIP340-canonical x-only) = 43 bytes`.

This entire tree is deterministic from `sk`, so the iOS wallet derives it once at first launch from the user's existing BIP39 seed and stores `ivk`/`ovk`/`dk` in `KeychainManager`.

#### `encrypted_note` byte layout

Per `docs/specs/shielded_derivation.md` §7:

```
encrypted_note total: 611 bytes
  epk (ephemeral pubkey):     32 bytes  (BIP340-canonical x-only)
  ct (ciphertext):           563 bytes
  tag (Poly1305):             16 bytes
```

Cleartext plaintext format (decrypted ct):

```
note_plaintext (563 bytes):
  version            1 byte    (0x01 for v030 schema)
  d (diversifier)   11 bytes
  value            8 bytes    (little-endian uint64, una)
  rcm              32 bytes   (commitment randomness)
  rseed            32 bytes   (note seed for esk reconstruction)
  memo            479 bytes   (free-form, optional)
```

Trial-decrypt:

```swift
func tryDecrypt(output: ShieldedOutput, ivk: Data) -> Note? {
    let epk = output.encryptedNote.prefix(32)
    let sharedSecret = sharedSecretFromECDH(ivk: ivk, epk: epk)
    let kEnc = hkdfDerive(sharedSecret, info: "DIN/v7/sh/enc/v1", len: 32)
    let nonce = hkdfDerive(sharedSecret, info: "DIN/v7/sh/nonce/v1", len: 12)
    let ct = output.encryptedNote.subdata(in: 32..<595)
    let tag = output.encryptedNote.suffix(16)

    guard let plaintext = chacha20poly1305Open(key: kEnc, nonce: nonce, ct: ct, tag: tag) else {
        return nil   // not for us; cheap fail
    }
    return Note.parse(plaintext, commitmentFromOutput: output.commitment, ivk: ivk)
}
```

Note: the exact KDF labels above are illustrative; the spec MUST adopt whatever labels the daemon's `shielded_derivation.cpp` actually uses. Read the daemon before porting.

#### Commitment-tree verification

After a successful decrypt, the iOS client MUST verify that the decrypted `(value, d, pk_d, rcm)` reproduces the on-chain `commitment` for that output:

```
commitment = Poseidon2(
    addr_bind=Poseidon2(ADDR_TAG, Poseidon2(d, pk_d)),
    value=value,
    rcm=rcm
)
```

where `ADDR_TAG = pad32("DIN/v7/shielded/addr/v1")`. If the recomputed commitment does not equal `output.commitment`, the decrypt is a false positive (statistically possible at AEAD tag collision rate, vanishingly rare).

This step also detects mis-encryption attacks where an adversary sends valid AEAD ciphertext to a victim with a commitment that doesn't bind to the same (value, recipient): the binding-sig validation on the daemon already prevents this from landing in a block, but the iOS client revalidates because it's cheap and the property is load-bearing.

#### Anchor verification

Each scanned block has a header hash. The client MUST:

1. Verify the block hash against its locally synced header chain (already done by `FilterChainSync.swift`).
2. Maintain a local incremental shielded commitment tree replica from `shielded_activation_height` forward, hashing every shielded output commitment into the frontier in canonical block order.
3. Verify that the block's reported `commitment_tree_root_after` value (verify the exact field name in `shielded_block_validation.cpp`) equals the locally derived root after appending that block's outputs.

**Endpoint divergence.** If two RPC endpoints disagree on `commitment_tree_root_after` for the same block hash, the client knows at least one is malicious or corrupt. M1 behavior:
- The endpoint whose reported root mismatches the local derivation is marked `untrusted_until_session_restart` in `NodeKit`'s per-endpoint health state.
- Scanner switches to the next healthy endpoint via the existing multi-endpoint fallback and retries from the last-verified snapshot height.
- If all endpoints diverge from the local derivation, the local derivation is the source of truth (the client has the headers and the block-of-interest data, and tree maintenance is deterministic). Surface a one-time alert: `Network inconsistency detected. Shielded sync paused until a trusted endpoint is available.` No funds at risk in receive-only M1; spend (M3) refuses to proceed in this state.

M1 commits to full tree maintenance. Trusting the block's reported root is explicitly out of scope: it leaves the client exposed to malicious-RPC wrong-`leaf_index` scenarios that may only surface later as Phase 2 witness/spend failures. With full local maintenance, scan-time failure is a clear chain inconsistency, stored `leaf_index` values are provably tied to the local anchor, and Phase 2 witness RPCs only need to prove a path against an anchor the client already owns.

#### Commitment-tree reorg handling

Sapling-shape incremental Merkle frontiers append cheaply but cannot pop. On a reorg the client cannot simply remove the disconnected blocks' leaves and continue — the frontier state above the new tip is invalid and must be reconstructed.

**Invariant (load-bearing for both M1 receive and M3 spend):** the client persists a verified frontier snapshot every `kSnapshotInterval = 256` blocks and retains the last `kSnapshotRetention = 8` snapshots at all times. This is **not an optimization** — it is the local anchor lineage that the witness verification in M3 will use to prove paths against. Reducing the retention, lengthening the interval, or skipping snapshots under storage pressure all break the ability to produce a valid spend witness after a reorg.

Concretely:

- `CommitmentTree.swift` persists a full frontier snapshot every 256 blocks (~8.5 hr at ~2 min/block, well above Dinero's policy-bounded reorg depth of 100 blocks).
- Snapshots are written atomically to `<datadir>/shielded_tree_snapshots/<height>.bin` and pruned to the last 8 entries (~2 days of rollback headroom). Retention below 8 is a correctness regression, not a tuning knob.
- On reorg detection (driven by `FilterChainSync`), the client picks the most recent snapshot at or below `new_tip - 100` (the deepest reorg the consensus allows), loads it as the active frontier, and re-applies blocks from snapshot height up to the new tip in canonical order.
- If no snapshot is recent enough (catastrophic deep reorg deeper than retention covers, or fresh wipe), fall back to full rebuild from `shielded_activation_height` — same code path the first-launch sync uses.

Snapshot file format mirrors the daemon's existing `anchor_history.bin` shape from `src/consensus/shielded/anchor_history.cpp`: `[magic 0xB0C30001][version 1][height][frontier-node-count][frontier-nodes...]`. Atomic via `path.tmp` rename. Failure of an individual snapshot write is non-fatal in the short term (the in-memory frontier remains valid until next snapshot interval), but repeated failures must be surfaced — silent loss of snapshot lineage degrades to "always full-rebuild on reorg" which is acceptable for receive-only M1 but breaks M3 spend.

**Cost:** one snapshot every 256 blocks × ~2 KB per snapshot (Sapling-shape frontier is O(log N) nodes, ~50 × 32 bytes at h=30k) = ~250 KB on disk for the 8-snapshot retention window. Replay after reorg: ≤256 blocks × ~1 ms tree work = ≤256 ms, well below user-visible threshold.

#### `ShieldedNoteDiscovery` orchestrator

Analog of `FilterBasedDiscovery.swift`:

```
1. Fetch headers + DNRF filters via FilterChainSync (existing).
2. For each block-of-interest range:
   a. Without daemon shielded filter: fetch full blocks (existing path) and enumerate shielded outputs.
   b. With daemon shielded filter (M2): fetch per-block diversifier filter; check
      our diversifiers against it; skip blocks with no match; fetch only matched blocks.
3. For each shielded output in each fetched block:
   - tryDecrypt(output, ivk) -> Note?
   - If decrypt succeeds: verify commitment, verify anchor, write to ShieldedNoteStore.
4. Mark notes as spent by scanning each block's spent-nullifier set against our owned-notes set.
```

#### `ShieldedNoteStore` persistence

A SQLite table (the app already uses SQLite for transparent UTXOs) keyed by `(txid, output_index)`. Columns:

```
txid TEXT,
output_index INTEGER,
commitment BLOB(32),
value_una INTEGER,
diversifier BLOB(11),
rcm BLOB(32),
rseed BLOB(32),
memo BLOB,
block_hash BLOB(32),
block_height INTEGER,
leaf_index INTEGER,        -- position in the commitment tree at scan time
nullifier BLOB(32),         -- precomputed via nsk
spent INTEGER DEFAULT 0,
spent_in_block_hash BLOB(32),
PRIMARY KEY (txid, output_index)
```

Reorg-safe: on reorg, the existing `FilterChainSync` rollback path rolls back transparent UTXOs from `UTXOStore`; the same path also calls into `ShieldedNoteStore` to truncate notes whose `block_height` is above the new tip.

Memo handling is store-only in M1: decrypt and persist the fixed-length memo blob, but do not surface it in the UI. M1.5 adds a narrow HistoryView detail expansion: row tap shows memo text in monospace up to 240 chars with a "show full" affordance. Markdown rendering and attachments stay out of scope; memos must not become a side channel.

#### Integration with existing UX

The existing `WalletShieldedView` already calls `wallet.listshielded` / `wallet.shieldedbalance` against the local embedded node. The wallet has a notion of mode (Embedded Node vs Shared Bridge RPC). The change here:

- In thin-client mode, `ShieldedNoteStore` becomes the source of truth — the `listshielded` / `shieldedbalance` RPC calls are replaced by local store reads.
- In Embedded Node mode, behavior is unchanged (existing RPCs still serve).
- The "Shared bridge RPC mode is intentionally blocked from deriving or spending shielded wallet funds" guardrail at `WalletShieldedView.swift:4890` is updated: bridge-RPC mode still cannot **spend** shielded (Phase 2 needs witness fetch), but **can now display** shielded balance + history from the local-only `ShieldedNoteStore`. The "Switch to Embedded Node" prompt is removed from receive-side flows.

### Phase 2 (spend)

To spend a discovered note, the client needs:

1. **The note's full witness:** Merkle path from the note's leaf to a recent anchor in the commitment tree. The local `ShieldedNoteStore` records `leaf_index`; the client fetches the path on demand via the new `shielded.witness.by_index <leaf_idx> <anchor_height>` RPC, then verifies the path against the locally-stored anchor (which was itself anchored at the PoW-checked block hash during scanning). **This step depends on the M1 snapshot-retention invariant above** — the witness verification anchor must come from a frontier snapshot the client computed itself, not from a server-reported root. Phase 2 spend cannot ship unless the M1 snapshot lineage is intact; treat that invariant as part of Phase 2's correctness contract, not as M1 polish.
2. **A Spartan spend proof.** Generated client-side. This requires porting the Spartan prover to Swift OR using a precompiled library. The daemon's Spartan/Hyrax/Nova prover is C++; expose it via a thin Swift bridge using SwiftPM + a vendored `.xcframework`. **This is the hardest part of Phase 2** and is roughly equivalent in scope to the rest of Phase 1 combined.
3. **A signed transaction envelope.** iOS already has `TransactionBuilder` + `TaprootSigner` for transparent; extend to wrap the shielded spend bundle + binding-sig.

Phase 2 is deferred to a separate spec once Phase 1 ships and we have real iOS user data.

## Daemon detail

### `blockchain.shielded.filter <hash>` RPC

Returns a GCS filter over the recipient-diversifier tags of all shielded outputs in the requested block.

For each shielded output `o` in a block:

```
tag = blake2s_personal(o.encrypted_note[0..32], "DIN/v7/sh/lcf/v1")[0..8]
```

(Use the ephemeral pubkey `epk` as the per-output identifier. The actual recipient diversifier is hidden inside the encrypted note; we cannot filter on it directly without leaking. But `epk` is unique per output and an iOS client can pre-compute `expected_epk_tag(d) = blake2s_personal(deriveExpectedEpk(d), tag)` — actually this doesn't work because `epk` is sender-chosen randomness, not deterministic from recipient diversifier. We need a different tag.)

**REVISED tag scheme:** the daemon computes
```
tag = blake2s_personal(pk_d, "DIN/v7/sh/lcf/v1")[0..8]
```
where `pk_d` is recovered from a successful decrypt — but the daemon doesn't know `pk_d` for arbitrary recipients. So this also doesn't work as a free filter.

**Open design question:** the recipient-diversifier filter requires the daemon to somehow tag each output in a way that clients can check against their `pk_d`s. Options:
1. **Tag = `H(ECDH(epk, view_key_hash_check_input))`** — requires sender to embed a public commitment per-recipient. Doesn't work without modifying the wire format.
2. **No daemon filter; iOS trial-decrypts every shielded output in every fetched block.** This is the M1 fallback. Cost: ~611 bytes per output × N outputs/block × Y blocks. For a typical home-user with one address, scanning 24 hrs of blocks (~720 blocks at ~2 min/block) with say 10 shielded outputs/block average = 7200 trial-decrypts/day = 1 sec of ChaCha20-Poly1305 on a modern iPhone. Tractable without the filter.
3. **Daemon publishes per-block "all `epk` values"** as a compact list (not a filter) — clients store their own derived ECDH outputs and intersect. Per-block storage cost: 32 bytes × outputs/block; not particularly cheap.

**Recommendation for M1: skip the daemon filter.** Trial-decrypt every shielded output. Cost is bounded and acceptable. Revisit only if real iOS users report scan-time pain.

### `blockchain.shielded.outputs <hash>` RPC

Returns just the shielded-output payload for a block, without the full block. This is a bandwidth optimization — full blocks include transparent txs that an iOS client may already have via the existing scan path. Returns:

```jsonc
{
  "block_hash": "...",
  "shielded_outputs": [
    { "txid": "...", "output_index": 0, "commitment": "...", "encrypted_note": "..." },
    ...
  ],
  "commitment_tree_root_after": "..."
}
```

Saves ~80% of block bandwidth on typical mainnet blocks. Low-risk addition; same shape as the existing `getblock` family.

### `shielded.witness.by_index <leaf_idx> <anchor_height>` RPC (Phase 2)

Returns a Merkle path from the requested leaf to the root at `anchor_height`. iOS verifies the path against its locally-stored anchor. Phase 2 only.

## Phasing

- **M1 (Phase 1 receive-only, no daemon changes):**
  iOS ports Poseidon, hash-to-curve, ChaCha20-Poly1305 (or adopts CryptoKit), HKDF, the shielded key tree, encrypted_note layout, commitment recompute, full incremental commitment-tree maintenance, and `ShieldedNoteDiscovery`. Uses existing `getblock` RPC. Trial-decrypts every shielded output in every block-of-interest. Scans the active wallet only. Surfaces balance + history in `WalletShieldedView` even in thin-client mode, with memos persisted but not rendered.
- **M1.5 (UX follow-up):**
  Add memo rendering in HistoryView detail and an explicit archived-wallet scan preference if users ask for it. Archived-wallet scanning is not part of M1.
- **M2 (bandwidth optimization, daemon-side):**
  Add `blockchain.shielded.outputs <hash>` RPC. iOS uses it in preference to `getblock` when available. ~80% bandwidth reduction. Optional but the daemon work is small (~150 LOC + tests).
- **M3 (Phase 2 spend):**
  Port the Spartan prover to a Swift-callable static library. Add `shielded.witness.by_index` RPC. Wire `wallet.shield` / `wallet.unshield` / `wallet.transfer` send paths in iOS thin-client mode.

M1 alone unlocks the home-user privacy thesis: an iPhone running thin-client mode can receive shielded payments. M3 unlocks symmetric privacy: receive **and** send without an embedded node.

## Performance + cost estimates

Estimates per the M1 design (trial-decrypt every shielded output):

- **Storage on iOS:** ~150 bytes per discovered note × N notes the wallet has received. ~15 KB for a thousand notes. Bounded and small.
- **CPU cost on iOS per block scanned:** dominated by the trial-decrypt × outputs-per-block.
  - ChaCha20-Poly1305 open: ~1 µs per 611-byte ciphertext on iPhone 16 (CryptoKit-accelerated).
  - ECDH on secp256k1: ~30 µs per output (existing Secp256k1.swift uses libsecp256k1 — verified fast).
  - Poseidon2 commit reverify on successful decrypt only: rare; ~100 µs.
  - Commitment-tree maintenance: ~1 ms/block to append every shielded output commitment and update the frontier.
  - Total per output: ~30 µs ECDH + ~1 µs AEAD = ~31 µs typical.
  - Per block (10 shielded outputs avg): ~310 µs trial-decrypt + ~1 ms tree maintenance.
- **Catchup time for a fresh wallet:** chain at h≈30k, `shielded_activation_height = 8650`, ~22k blocks, ~10 outputs/block average → ~220k-300k trial-decrypts (~10 sec) plus ~22 sec full tree maintenance. Budget ~32 sec on first sync **under today's mainnet conditions (light shielded adoption).** Once mobile thin-client mode unblocks shielded adoption — the explicit goal of this work — outputs/block grows and catchup grows linearly. A wallet recovering from wipe 12-18 months after broad activation could see 3-10× more outputs/block; first-sync budgeting should plan for ~3-5 minutes under those conditions. Mitigations available without spec changes: M2 bandwidth optimization reduces I/O; the `.xcframework` Poseidon mitigation reduces CPU; both compose. Acceptable for a one-time restore because it runs with progress UI and can continue in background.
- **Steady-state scan cost:** ~1.3 ms per 2-min block = essentially zero battery impact.
- **Network bandwidth:** full block fetch ~50 KB/block × N blocks. ~36 MB to catch up the whole chain at h=30k. With M2 (`shielded.outputs`), drops to ~7 MB.

These numbers comfortably fit within mobile resource budgets.

## Test plan

### iOS unit tests

- `PoseidonTests`: 8 vectors from `docs/specs/shielded_v030_test_vectors.md` round-trip.
- `HashToCurveTests`: 4 vectors per RFC 9380 §J.8 + dinero-specific DST.
- `ChaCha20Poly1305Tests`: encrypt-then-decrypt round-trip + RFC 8439 vectors.
- `ShieldedKeyDerivationTests`: from BIP39 seed → ivk/ovk → known address. Vectors generated by daemon and committed to `docs/specs/shielded_v030_test_vectors.md`.
- `NoteCipherTests`: trial-decrypt a known-good ciphertext succeeds; same ciphertext with wrong ivk fails; AEAD-tampered ciphertext fails with correct ivk.
- `CommitmentRecomputeTests`: ports the equivalent of `src/test/pedersen_tests.cpp::CommitmentReproducibility`.
- `ShieldedNoteStoreTests`: write, read, mark-spent, rollback-on-reorg.
- `CommitmentTreeSnapshotTests`: append N=1000 leaves, snapshot at intervals, simulate reorg back 80 blocks, replay forward, assert frontier-root matches a pristine rebuild. Repeat at boundaries (h%256==0, h%256==255).

### iOS integration tests

- `ShieldedScannerEndToEndTest`: spin up a regtest dinerod (via fixtures), shield 1 DIN to an iOS-derived address, run `ShieldedNoteDiscovery`, assert the note is discovered with correct value + commitment + nullifier.
- `ShieldedScannerReorgTest`: shield + reorg-away the block, run scanner, assert the note is removed from `ShieldedNoteStore`.
- `ShieldedScannerNoFalsePositivesTest`: shield 100 notes to other recipients, scan with our ivk, assert zero discovered (statistical AEAD false-positive rate; 100 trials is well below detection threshold).

### Daemon tests (M2)

- `BlockchainShieldedOutputsRpcTest`: regtest scenario shields 5 notes, calls `blockchain.shielded.outputs <hash>`, asserts it returns exactly those 5 outputs with correct `commitment` + `encrypted_note` bytes.
- `BlockchainShieldedOutputsBandwidthTest`: assert payload size is < 20% of equivalent `getblock` result.

## M1 decisions

1. **CryptoKit compatibility is a day-1 spike, not a blocker.** `ChaChaPoly` + `HKDF<SHA256>` should cover the daemon path directly; verify the exact `kAeadInfo`, salt, nonce, AAD, and ciphertext/tag layout first. Fall back to a small pure-Swift AEAD port only if byte parity fails.
2. **Anchor verification uses full incremental tree maintenance.** M1 does not trust the block's reported root. The client appends every shielded output commitment locally and refuses to advance on root mismatch.
3. **Wallet scope is active-wallet-only.** This matches existing transparent `FilterBasedDiscovery` behavior. Archived wallet/account scanning is deferred to M1.5 behind a user preference if demand appears.
4. **Memos are store-only in M1.** The scanner persists memo blobs in `ShieldedNoteStore`; UI rendering is M1.5.
5. **Recovery-from-wipe requires progress UI.** During restore/rescan, show a non-modal banner: `Scanning shielded history... block 12,847 / 22,103 (58%)`. Users can dismiss the banner into background; scanning continues in the existing iOS `BGProcessing` slot already used by `FilterChainSync`. Transparent balance, send, and history remain usable. On completion, show a one-time toast: `Shielded ready - N notes synced`.

## File map

- New: `DineroDPI/DineroDPI/Core/Crypto/Poseidon.swift` (~400 LOC port)
- New: `DineroDPI/DineroDPI/Core/Crypto/HashToCurve.swift` (~200 LOC)
- New: `DineroDPI/DineroDPI/Core/Crypto/ChaCha20Poly1305.swift` (~80 LOC, CryptoKit wrapper)
- New: `DineroDPI/DineroDPI/Core/Crypto/HKDF.swift` (~30 LOC, CryptoKit wrapper)
- New: `DineroDPI/DineroDPI/Core/Shielded/ShieldedKeys.swift` (~300 LOC)
- New: `DineroDPI/DineroDPI/Core/Shielded/ShieldedAddress.swift` (~150 LOC)
- New: `DineroDPI/DineroDPI/Core/Shielded/NoteCipher.swift` (~200 LOC)
- New: `DineroDPI/DineroDPI/Core/Shielded/CommitmentTree.swift` (~350 LOC — frontier maintenance + snapshot persistence + reorg replay)
- New: `DineroDPI/DineroDPI/Core/Shielded/AnchorVerifier.swift` (~100 LOC)
- New: `DineroDPI/DineroDPI/Core/Shielded/ShieldedNote.swift` (~80 LOC value type)
- New: `DineroDPI/DineroDPI/Core/Filters/ShieldedNoteDiscovery.swift` (~300 LOC)
- New: `DineroDPI/DineroDPI/Core/Stores/ShieldedNoteStore.swift` (~250 LOC)
- Modify: `DineroDPI/DineroDPI/Core/Crypto/BlockParser.swift` — add shielded output enumeration
- Modify: `DineroDPI/DineroDPI/Views/WalletView.swift` — `WalletShieldedView` reads from `ShieldedNoteStore` in thin-client mode
- Modify: `DineroDPI/DineroDPI/DineroDPIApp.swift` — orchestrate `ShieldedNoteDiscovery` alongside `FilterBasedDiscovery` in the sync cycle
- New tests: ~10 unit + 3 integration test files

iOS total: ~2300 LOC new + ~150 LOC modified. Comparable in size to the Phase 2b qt PR but more crypto-heavy.

Daemon M2 (optional):
- New: `include/rpc/shielded_lightclient_rpc.h` (~25 LOC)
- New: `src/rpc/shielded_lightclient_rpc.cpp` (~200 LOC)
- Modify: `src/daemon/rpc_context_wiring.cpp` (~5 LOC)
- New: `tests/network/test_shielded_outputs_rpc.sh` (~120 LOC)

Daemon total (M2): ~350 LOC.

## Coverage map (self-review)

| Goal | Where covered |
|---|---|
| Shielded balance in iOS thin-client mode | `ShieldedNoteStore` + `WalletShieldedView` integration |
| Shielded receive in iOS thin-client mode | `ShieldedNoteDiscovery` + `NoteCipher` trial-decrypt |
| Privacy property: server doesn't see decrypts | Client-only trial-decrypt; no view-key transmission |
| Privacy property: server doesn't see view key | `KeychainManager` retains ivk; only fetches blocks |
| Anchor / chain-validity check | `AnchorVerifier` + `FilterChainSync` (existing) |
| Reorg safety | `ShieldedNoteStore` rollback hooked into `FilterChainSync` |
| Multi-wallet support | "Active wallet only" by default; opt-in scan all (Q3) |
| Performance | Per-block ~310µs; full chain catchup ~10s |
| M3 spend path | Future spec; this design lays groundwork (witness-by-index RPC noted) |

## Why this is uniquely Dinero's

No other production chain has shipped trustless light-client privacy that actually preserves privacy:

- **Bitcoin** has no shielded primitive.
- **Zcash** has `lightwalletd`, but the server learns the client's scan-key derivation hints; the client's privacy depends on trusting a specific server operator. The cypherpunk story is broken.
- **Monero** has view-key light-client mode, but outsourcing the view key surrenders all read privacy to whoever holds it. The threat model and the trust model are the same person.
- **Aztec** has client-side proving but no notion of "light client" — full node prover required.

Dinero is the only chain whose substrate composes:
1. Live shielded pool with classical-curve crypto that's portable to mobile (secp256k1, ChaCha20, Poseidon-2 over secp256k1 — no exotic Jubjub/Pallas curve needed).
2. Transparent-setup ZK (Spartan/Hyrax) — no trusted ceremony, so client-side verification is unambiguously sound.
3. DNRF compact block filters already running on iOS — gives us free per-block filtering for the transparent-discovery path that we extend, not reinvent.
4. A small, single-team codebase where both the daemon and the iOS client are maintained together — protocol-level changes (M2 RPC) can be coordinated with client-side implementation in days, not years.

This spec ships a feature no other crypto project has shipped, by composing primitives that all individually already exist. The work is real engineering — not research — but the design has no open theoretical questions.
