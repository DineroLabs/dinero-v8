#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 5 Wave 1 — Shielded key derivation (Sapling-shape, secp256k1-flavored).
//
// Implements the wallet-only portion of `docs/specs/shielded_derivation.md`
// up through §4 (incoming viewing key). Diversifier generation, hash-to-curve
// for `pk_d`, and bech32m address encoding are subsequent waves.
//
// Inputs:  BIP39-style seed bytes (typically 64) + account index.
// Outputs: ShieldedAccountKeys with sk, ask, nsk, ovk, dk, ak, nk, ivk —
//          all 32-byte hashes (Sapling's IVK lives in the secp256k1 scalar
//          field, encoded big-endian to match commitment_tree's existing
//          PoseidonHash2 byte convention).
//
// Spec divergence note (resolved in favor of code): the spec text §4.2
// describes scalar interpretation as little-endian; the existing
// `commitment_tree.cpp` PoseidonHash2 implementation interprets bytes as
// big-endian. We follow the code (per memo §0: "consensus code wins"),
// and a separate spec PR will align §4.2 with the on-chain convention.
// ═══════════════════════════════════════════════════════════════════════════════

#include "consensus/shielded/commitment_tree.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>

namespace dinero::wallet::shielded {

using consensus::shielded::Hash;

/// Domain-separation tags. Versioned so a future v2 spec can coexist.
constexpr const char* kDstAsk = "DIN/v7/shielded/ask";
constexpr const char* kDstNsk = "DIN/v7/shielded/nsk";
constexpr const char* kDstOvk = "DIN/v7/shielded/ovk";
constexpr const char* kDstDk  = "DIN/v7/shielded/dk";

/// BIP32 path components for shielded derivation: m/99'/1448'/account'.
/// Purpose 99 separates shielded from BIP86 Taproot (purpose 86) and
/// BIP88 P2MR (purpose 88) under the same coin_type 1448.
constexpr uint32_t kPurposeShielded = 99;
constexpr uint32_t kCoinTypeDinero  = 1448;

/// One account's worth of shielded keys. All public components are encoded
/// as 32-byte x-only big-endian (BIP340 convention).
struct ShieldedAccountKeys {
    /// Master secret at m/99'/1448'/account'. Same shape as a BIP32
    /// child private key (32 bytes).
    Hash sk{};

    /// Spend authority key. Scalar in [1, q-1]. ak = ask·G has even-y by
    /// the BIP340 normalisation: if ask·G originally had odd-y, ask is
    /// negated so the encoded ak is the even-y representative.
    Hash ask{};

    /// Nullifier key. Same shape and normalisation as ask/ak.
    Hash nsk{};

    /// Outgoing viewing key — symmetric, no scalar interpretation.
    /// Decrypts notes the wallet sent; useful for backup recovery.
    Hash ovk{};

    /// Diversifier key — ChaCha20 key for the deterministic diversifier
    /// stream. Wave 2 consumes this.
    Hash dk{};

    /// Public spend key. ak = (ask · G) x-only, even-y.
    Hash ak{};

    /// Public nullifier key. nk = (nsk · G) x-only, even-y.
    Hash nk{};

    /// Incoming viewing key. ivk = Poseidon2(ak, nk) reduced mod q.
    /// ivk·P_d will yield each diversified address's pk_d (Wave 2).
    Hash ivk{};
};

/// Derive the full shielded key bundle at m/99'/1448'/account'.
/// `seed` must be a BIP32-shaped seed (typically 64 bytes from BIP39).
/// Throws std::runtime_error on BIP32 failure or if PRF outputs hit zero
/// (negligible probability).
ShieldedAccountKeys DeriveShieldedAccount(const uint8_t* seed,
                                          std::size_t seed_len,
                                          uint32_t account);

// ── Internal helpers, exposed for test-vector pinning ─────────────────

/// Pack a NUL-terminated ASCII DST string into a 32-byte buffer with
/// the string at the high end (byte 0 = first DST char) and zero
/// padding at the tail. Matches `AddrBindTag()` in commitment_tree.cpp.
Hash DstToHash(const char* dst);

/// Spec §4.2 PRF. Returns Poseidon2(key, DstToHash(dst)) — output is a
/// 32-byte big-endian scalar representative ready to be reduced mod q
/// or used as a symmetric key, depending on the caller's role.
Hash ShieldedPRF(const Hash& key, const char* dst);

// ── Phase 5 Wave 2: diversifier + diversified address ────────────────

/// 11-byte raw diversifier (§5.1).
using Diversifier = std::array<uint8_t, 11>;

/// 43-byte address payload: d (11) || pk_d (32) (§5.4).
using AddressPayload = std::array<uint8_t, 43>;

/// Domain-separation tag for hash-to-point in §5.2.
constexpr const char* kDstDiv = "DIN/v7/shielded/div";

/// Network HRPs (§5.5).
constexpr const char* kHrpMainnet = "dins";
constexpr const char* kHrpTestnet = "tdins";
constexpr const char* kHrpRegtest = "rdins";

/// Diversifier from `dk` and 88-bit index `j`. Implements §5.1 with a
/// 12-byte ChaCha20 nonce = j_little_endian zero-padded to 12 bytes,
/// counter = 0. Returns the first 11 bytes of the keystream.
Diversifier ChaCha20Diversifier(const Hash& dk, uint64_t j);

/// Hash-to-point for `P_d`. Spec §5.2 calls for RFC 9380 SSWU_RO; this
/// implementation deviates to match the V-generator pattern in
/// `pedersen_generators.cpp`: seed = SHA-256(`dst` || d || zero-pad to
/// 32 bytes), then `secp256k1_generator_generate(ctx, &P, seed)`. Same
/// nothing-up-my-sleeve property; same negligible collision probability;
/// no separate SSWU implementation required. A future spec PR will
/// align §5.2 with the implementation. Returns x-only 32 bytes.
Hash HashToPoint(const Diversifier& d, const char* dst);

/// `pk_d = ivk · P_d` with even-y normalisation (§5.3). `p_d_xonly` is
/// the 32-byte even-y x-coordinate of `P_d`. Output is 32-byte even-y.
Hash DerivePkD(const Hash& ivk, const Hash& p_d_xonly);

/// Build the 43-byte address payload `d || pk_d`.
AddressPayload BuildAddressPayload(const Diversifier& d, const Hash& pk_d);

/// Bech32m-encode the 43-byte payload under `hrp`. No witness-version
/// prefix; raw payload bytes converted 8→5 then bech32m-encoded.
std::string EncodeShieldedAddress(const AddressPayload& payload,
                                  const std::string& hrp);

/// One diversified address.
struct DiversifiedAddress {
    Diversifier d{};
    Hash        pk_d{};
    AddressPayload payload{};
    std::string address;  // bech32m string for the chosen HRP
};

/// Compute the diversified address at index `j` for the supplied keys.
/// `hrp` should be `kHrpMainnet` / `kHrpTestnet` / `kHrpRegtest`.
DiversifiedAddress DeriveDiversifiedAddress(const ShieldedAccountKeys& keys,
                                            uint64_t j,
                                            const std::string& hrp);

// ── Phase 5 Wave 3: address decode + encrypted-note ECDH/AEAD ────────

/// Result of `DecodeShieldedAddress`. The HRP is returned so the
/// daemon can enforce network-match per spec §7.1.
struct DecodedShieldedAddress {
    std::string    hrp;
    Diversifier    d{};
    Hash           pk_d{};
    AddressPayload payload{};
};

/// Decode a `dins` / `tdins` / `rdins` bech32m string back to its
/// `(d, pk_d)` components. Validates: bech32m checksum (rejects
/// bech32 v1), 43-byte payload length, and `pk_d` x-coordinate is on
/// the secp256k1 curve. Throws `std::runtime_error` on any failure.
DecodedShieldedAddress DecodeShieldedAddress(const std::string& addr);

/// Spec §6.1 plaintext note: 11 + 8 + 32 + 512 = 563 bytes.
struct NotePlaintext {
    Diversifier            d{};
    uint64_t               value_una = 0;
    Hash                   rcm{};
    std::array<uint8_t, 512> memo{};

    /// Serialize to the 563-byte canonical layout (LE value, fixed memo).
    std::array<uint8_t, 563> Serialize() const;

    /// Inverse of Serialize.
    static NotePlaintext Deserialize(const std::array<uint8_t, 563>& bytes);
};

/// 611-byte encrypted note: epk (32) || ChaCha20-Poly1305 ciphertext
/// (563 + 16 tag).
constexpr std::size_t kEncryptedNoteBytes = 611;
using EncryptedNote = std::array<uint8_t, kEncryptedNoteBytes>;

/// Encrypt `note` to recipient's `(d, pk_d)`. `epk` is derived as
/// `esk · P_d` per Sapling (spec §6.3 says `esk · G` but that is
/// internally inconsistent with the `pk_d = ivk · P_d` derivation in
/// §5.3 — sender and receiver cannot agree on `shared` without `epk`
/// living on the diversified base `P_d`). Documented inline + spec PR
/// pending. `note.d` MUST equal the recipient's `d` so the receiver's
/// post-decrypt `pk_d == ivk·HashToPoint(d)` check passes.
///
/// `esk_override` is optional and only used for deterministic test
/// vectors; in production callers MUST pass nullptr to use a fresh
/// random esk per output (zero-knowledge unlinkability requires this).
EncryptedNote EncryptNoteForRecipient(const Diversifier& recipient_d,
                                      const Hash& pk_d_xonly,
                                      const NotePlaintext& note,
                                      const Hash* esk_override = nullptr);

/// Try to decrypt with `ivk`. Returns std::nullopt on AEAD failure
/// (note is not for this wallet, or ciphertext was tampered).
std::optional<NotePlaintext> TryDecryptNoteForViewer(
    const Hash& ivk,
    const EncryptedNote& encrypted);

/// Per-note spend-key derivation. The sender chooses a fresh `rcm` for
/// every output note; both sender and receiver derive the per-note
/// spend secret deterministically from `rcm` so the spend secret never
/// has to travel out-of-band:
///
///   sk_note = Poseidon2(rcm, DstToHash("DIN/v7/shielded/sk_note/v1"))
///   pk_note = Poseidon2(sk_note, 0)
///
/// The on-chain `commitment = NoteCommitment(d, pk_note, value, rcm)`
/// uses this `pk_note`. Receiver, after AEAD-decrypting the encrypted
/// note plaintext to recover `rcm`, re-derives `sk_note` and `pk_note`
/// and verifies the commitment matches before persisting the note.
///
/// Spec note: this is a Dinero-specific deviation from Sapling's
/// "ask is a wallet-wide spend authority" model. The Dinero shielded
/// circuit verifies `pk == Poseidon(sk, 0)` per spend, so each note
/// has its own (sk, pk) pair. Deriving sk from rcm preserves the
/// existing circuit while enabling any-recipient transfer.
constexpr const char* kDstNoteSpendKey = "DIN/v7/shielded/sk_note/v1";

Hash DeriveNoteSpendKey(const Hash& rcm);

}  // namespace dinero::wallet::shielded
