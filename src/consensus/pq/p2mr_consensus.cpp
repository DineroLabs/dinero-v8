/*
 * V7 P2MR consensus primitives — implementation.
 * See include/consensus/pq/p2mr_consensus.h.
 *
 * This file contains the serialize/deserialize/merkle/verify logic. It is
 * pure in the inputs — no I/O, no randomness, no wall clock. Bit-identical
 * output across architectures is a consensus requirement; any drift here
 * is a fork.
 */

#include "consensus/pq/p2mr_consensus.h"
#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/scheme_registry.h"
#include "consensus/utxo_entry.h"
#include "crypto/sha256.h"
#include "primitives/transaction.h"

#include <cstring>

namespace dinero::consensus::pq {

// -----------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------

namespace {

std::array<uint8_t, 32> Sha256Concat(const uint8_t* a, std::size_t a_len,
                                     const uint8_t* b, std::size_t b_len) {
    std::array<uint8_t, 32> out{};
    dinero::crypto::CSHA256()
        .Write(a, a_len)
        .Write(b, b_len)
        .Finalize(out.data());
    return out;
}

// CompactSize varint encode (Bitcoin style). Writes into `out` which must
// have at least 9 bytes of capacity. Returns number of bytes written.
std::size_t EncodeCompactSize(uint64_t v, uint8_t out[9]) {
    if (v < 0xfd) {
        out[0] = static_cast<uint8_t>(v);
        return 1;
    }
    if (v <= 0xffff) {
        out[0] = 0xfd;
        out[1] = static_cast<uint8_t>(v & 0xff);
        out[2] = static_cast<uint8_t>((v >> 8) & 0xff);
        return 3;
    }
    if (v <= 0xffffffffULL) {
        out[0] = 0xfe;
        for (int i = 0; i < 4; ++i) out[1 + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
        return 5;
    }
    out[0] = 0xff;
    for (int i = 0; i < 8; ++i) out[1 + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
    return 9;
}

// CompactSize varint decode. On success, writes decoded value to *v and
// returns number of bytes consumed (>0). On truncation/invalid input,
// returns 0.
std::size_t DecodeCompactSize(const uint8_t* bytes, std::size_t len, uint64_t* v) {
    if (len == 0) return 0;
    const uint8_t first = bytes[0];
    if (first < 0xfd) { *v = first; return 1; }
    if (first == 0xfd) {
        if (len < 3) return 0;
        *v = (uint64_t)bytes[1] | ((uint64_t)bytes[2] << 8);
        return 3;
    }
    if (first == 0xfe) {
        if (len < 5) return 0;
        uint64_t x = 0;
        for (int i = 0; i < 4; ++i) x |= (uint64_t)bytes[1 + i] << (8 * i);
        *v = x;
        return 5;
    }
    // first == 0xff
    if (len < 9) return 0;
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= (uint64_t)bytes[1 + i] << (8 * i);
    *v = x;
    return 9;
}

} // namespace

// -----------------------------------------------------------------------
// Script recognition
// -----------------------------------------------------------------------

bool IsP2MRScript(const uint8_t* script, std::size_t len) noexcept {
    if (len != P2MR_SCRIPT_BYTES) return false;
    if (script[0] != P2MR_OP_WITNESS_V3) return false;
    if (script[1] != P2MR_OP_PUSH32)     return false;
    return true;
}

bool IsP2MRScript(const std::vector<uint8_t>& script) noexcept {
    return IsP2MRScript(script.data(), script.size());
}

std::optional<std::array<uint8_t, P2MR_ROOT_BYTES>>
ExtractP2MRMerkleRoot(const std::vector<uint8_t>& script) noexcept {
    if (!IsP2MRScript(script)) return std::nullopt;
    std::array<uint8_t, P2MR_ROOT_BYTES> root{};
    std::memcpy(root.data(), script.data() + 2, P2MR_ROOT_BYTES);
    return root;
}

// -----------------------------------------------------------------------
// Witness codec
// -----------------------------------------------------------------------

std::vector<uint8_t> SerializeP2MRWitness(const P2MRWitness& w) {
    // Pre-flight validation: return empty on structural issues so
    // callers cannot accidentally produce a witness that would fail to
    // round-trip through the decoder.
    if (w.merkle_depth > P2MR_MAX_MERKLE_DEPTH) return {};
    if (w.sibling_hashes.size() != w.merkle_depth) return {};
    // Leaf index must fit in 2^depth. For depth==0, only leaf_index==0 is valid.
    if (w.merkle_depth == 0) {
        if (w.leaf_index != 0) return {};
    } else if (w.leaf_index >= (uint64_t{1} << w.merkle_depth)) {
        return {};
    }

    std::vector<uint8_t> out;
    // Reserve generously; exact size isn't critical for a few KB.
    out.reserve(1 + 3 + w.pubkey_bytes.size() + 3 + w.signature_bytes.size()
                + 1 + w.merkle_depth * 32 + 9);

    // scheme_id (1 byte)
    out.push_back(w.scheme_id);

    // pubkey_len (varint) || pubkey
    uint8_t vbuf[9];
    std::size_t vlen = EncodeCompactSize(w.pubkey_bytes.size(), vbuf);
    out.insert(out.end(), vbuf, vbuf + vlen);
    out.insert(out.end(), w.pubkey_bytes.begin(), w.pubkey_bytes.end());

    // sig_len (varint) || sig
    vlen = EncodeCompactSize(w.signature_bytes.size(), vbuf);
    out.insert(out.end(), vbuf, vbuf + vlen);
    out.insert(out.end(), w.signature_bytes.begin(), w.signature_bytes.end());

    // merkle_depth (1 byte)
    out.push_back(w.merkle_depth);

    // sibling_hashes (depth * 32 bytes)
    for (const auto& sib : w.sibling_hashes) {
        out.insert(out.end(), sib.begin(), sib.end());
    }

    // leaf_index (varint)
    vlen = EncodeCompactSize(w.leaf_index, vbuf);
    out.insert(out.end(), vbuf, vbuf + vlen);

    return out;
}

P2MRWitnessDecodeError DeserializeP2MRWitness(const uint8_t* bytes,
                                              std::size_t    len,
                                              P2MRWitness*   out) {
    if (len == 0) return P2MRWitnessDecodeError::Truncated;

    P2MRWitness w{};
    std::size_t off = 0;

    // scheme_id
    w.scheme_id = bytes[off++];
    const auto& row = GetSchemeParams(w.scheme_id);
    // The registry gate (IsSchemeAcceptedAtHeight) is applied separately
    // by VerifyP2MRSpend; here we only demand structural sanity. But we
    // do reject Reserved rows outright — they carry zero byte caps and
    // trying to length-check against them is meaningless.
    if (row.state == SchemeState::Reserved) {
        return P2MRWitnessDecodeError::UnknownScheme;
    }
    // Every PQ scheme we register (ACCEPT or DARK_RESERVED) has a
    // fixed-length pubkey and signature; the registry's *_max fields
    // hold that fixed length. We treat them as exact-equals rather
    // than upper bounds — canonical encoding requires ONE length for
    // a given scheme_id, not a range. Any future variable-length
    // scheme (unlikely for PQ) would need this check relaxed and a
    // min/max pair on the registry row.
    const uint32_t pubkey_fixed_len = row.pubkey_bytes_max;
    const uint32_t sig_fixed_len    = row.signature_bytes_max;

    // pubkey_len + pubkey
    uint64_t pubkey_len = 0;
    std::size_t consumed = DecodeCompactSize(bytes + off, len - off, &pubkey_len);
    if (consumed == 0) return P2MRWitnessDecodeError::Truncated;
    off += consumed;
    if (pubkey_len != pubkey_fixed_len) {
        return P2MRWitnessDecodeError::PubkeyLenInvalid;
    }
    if (len - off < pubkey_len) return P2MRWitnessDecodeError::Truncated;
    w.pubkey_bytes.assign(bytes + off, bytes + off + pubkey_len);
    off += static_cast<std::size_t>(pubkey_len);

    // sig_len + sig
    uint64_t sig_len = 0;
    consumed = DecodeCompactSize(bytes + off, len - off, &sig_len);
    if (consumed == 0) return P2MRWitnessDecodeError::Truncated;
    off += consumed;
    if (sig_len != sig_fixed_len) {
        return P2MRWitnessDecodeError::SignatureLenInvalid;
    }
    if (len - off < sig_len) return P2MRWitnessDecodeError::Truncated;
    w.signature_bytes.assign(bytes + off, bytes + off + sig_len);
    off += static_cast<std::size_t>(sig_len);

    // merkle_depth
    if (len - off < 1) return P2MRWitnessDecodeError::Truncated;
    w.merkle_depth = bytes[off++];
    if (w.merkle_depth > P2MR_MAX_MERKLE_DEPTH) {
        return P2MRWitnessDecodeError::MerkleDepthTooDeep;
    }

    // sibling_hashes (depth * 32 bytes)
    const std::size_t sibs_bytes = static_cast<std::size_t>(w.merkle_depth) * 32;
    if (len - off < sibs_bytes) return P2MRWitnessDecodeError::Truncated;
    w.sibling_hashes.resize(w.merkle_depth);
    for (uint8_t i = 0; i < w.merkle_depth; ++i) {
        std::memcpy(w.sibling_hashes[i].data(), bytes + off, 32);
        off += 32;
    }

    // leaf_index
    uint64_t leaf_index = 0;
    consumed = DecodeCompactSize(bytes + off, len - off, &leaf_index);
    if (consumed == 0) return P2MRWitnessDecodeError::Truncated;
    off += consumed;
    if (w.merkle_depth == 0) {
        if (leaf_index != 0) return P2MRWitnessDecodeError::LeafIndexOutOfRange;
    } else if (leaf_index >= (uint64_t{1} << w.merkle_depth)) {
        return P2MRWitnessDecodeError::LeafIndexOutOfRange;
    }
    w.leaf_index = leaf_index;

    // No trailing bytes allowed.
    if (off != len) return P2MRWitnessDecodeError::TrailingBytes;

    // Canonicalization gate: the decoded struct must re-serialize to the
    // exact input byte sequence. Every per-field check above is a
    // necessary condition for canonicity; this is the sufficient one.
    // It catches non-minimal CompactSize encodings (0xfd 0x05 0x00 for
    // value 5, etc.) and any future parser slack we'd otherwise have
    // to enumerate. Consensus rule: one valid spend = one valid byte
    // encoding.
    const std::vector<uint8_t> canonical = SerializeP2MRWitness(w);
    if (canonical.size() != len ||
        std::memcmp(canonical.data(), bytes, len) != 0) {
        return P2MRWitnessDecodeError::NonCanonical;
    }

    *out = std::move(w);
    return P2MRWitnessDecodeError::Ok;
}

// -----------------------------------------------------------------------
// Merkle verification
// -----------------------------------------------------------------------

std::array<uint8_t, 32> ComputeP2MRLeafHash(uint8_t              scheme_id,
                                            const uint8_t*       pubkey_bytes,
                                            std::size_t          pubkey_len) {
    std::array<uint8_t, 32> out{};
    dinero::crypto::CSHA256()
        .Write(&scheme_id, 1)
        .Write(pubkey_bytes, pubkey_len)
        .Finalize(out.data());
    return out;
}

std::optional<std::array<uint8_t, 32>>
ComputeMerkleRoot(const std::array<uint8_t, 32>&             leaf_hash,
                  uint8_t                                    depth,
                  const std::vector<std::array<uint8_t, 32>>& sibling_hashes,
                  uint64_t                                   leaf_index) {
    if (depth > P2MR_MAX_MERKLE_DEPTH) return std::nullopt;
    if (sibling_hashes.size() != depth) return std::nullopt;
    if (depth == 0) {
        if (leaf_index != 0) return std::nullopt;
        return leaf_hash;
    }
    if (leaf_index >= (uint64_t{1} << depth)) return std::nullopt;

    std::array<uint8_t, 32> h = leaf_hash;
    for (uint8_t i = 0; i < depth; ++i) {
        const bool go_right = ((leaf_index >> i) & 1ULL) == 1ULL;
        const std::array<uint8_t, 32>& sib = sibling_hashes[i];
        if (go_right) {
            // leaf is right child; sibling is left
            h = Sha256Concat(sib.data(), 32, h.data(), 32);
        } else {
            // leaf is left child; sibling is right
            h = Sha256Concat(h.data(), 32, sib.data(), 32);
        }
    }
    return h;
}

// -----------------------------------------------------------------------
// End-to-end spend verifier
// -----------------------------------------------------------------------

P2MRVerifyError VerifyP2MRSpend(const std::vector<uint8_t>&    script_pubkey,
                                const std::vector<uint8_t>&    witness_bytes,
                                const std::array<uint8_t, 32>& sighash,
                                uint32_t                       height) {
    // 1. Script shape.
    auto commitment = ExtractP2MRMerkleRoot(script_pubkey);
    if (!commitment) return P2MRVerifyError::BadScriptShape;

    // 2. Decode witness.
    P2MRWitness w{};
    auto decode_rc = DeserializeP2MRWitness(witness_bytes, &w);
    if (decode_rc != P2MRWitnessDecodeError::Ok) {
        return P2MRVerifyError::WitnessDecodeFailed;
    }

    // 3. Registry gate.
    if (!IsSchemeAcceptedAtHeight(w.scheme_id, height)) {
        return P2MRVerifyError::SchemeNotAcceptedHere;
    }

    // 4. PQ signature verify.
    //    The message is the 32-byte sighash — we pass those bytes directly
    //    into ml_dsa_65::Verify. The signature scheme implementation
    //    handles internal hashing per FIPS 204.
    //    (Currently only ML-DSA-65 / scheme_id=0x01 is ACCEPT. When
    //    FALCON or SPHINCS+ activate, extend here with a dispatch switch
    //    on scheme_id.)
    if (w.scheme_id == SCHEME_ID_ML_DSA_65) {
        const bool sig_ok = ml_dsa_65::Verify(
            sighash.data(),           sighash.size(),
            w.signature_bytes.data(), w.signature_bytes.size(),
            w.pubkey_bytes.data(),    w.pubkey_bytes.size());
        if (!sig_ok) return P2MRVerifyError::SignatureInvalid;
    } else {
        // Registry gate above already rejects DarkReserved / Reserved rows.
        // Reaching this path for an unknown ACCEPT scheme means a future
        // scheme was activated without adding its verifier. Treat as
        // internal error — correct behavior is to never let this happen.
        return P2MRVerifyError::InternalError;
    }

    // 5. Merkle path check.
    const auto leaf = ComputeP2MRLeafHash(w.scheme_id,
                                          w.pubkey_bytes.data(),
                                          w.pubkey_bytes.size());
    auto recomputed = ComputeMerkleRoot(leaf, w.merkle_depth,
                                        w.sibling_hashes, w.leaf_index);
    if (!recomputed) return P2MRVerifyError::InternalError;
    if (*recomputed != *commitment) return P2MRVerifyError::MerklePathMismatch;

    return P2MRVerifyError::Ok;
}

} // namespace dinero::consensus::pq

// ---------------------------------------------------------------------------
// VWU — Verification Weight Units
// ---------------------------------------------------------------------------

namespace dinero::consensus {

std::optional<uint64_t>
ComputeVWU(const dinero::Transaction& tx,
           const std::vector<UTXOEntry>& prevouts) {
    // Base charge: stripped transaction size (no witness). This matches
    // BIP141's "base size" — the on-chain bytes every node must store
    // and forward independent of whether witness is pruned.
    const uint64_t stripped = static_cast<uint64_t>(tx.GetBaseSize());
    uint64_t vwu = stripped;

    // Coinbase shortcut: no inputs to charge. Mirrors how other fee
    // paths skip the coinbase (it has no prevout, no signature, nothing
    // to price).
    if (tx.IsCoinbase()) {
        return vwu;
    }

    // Per-input cost. The prevouts vector must align with tx.vin 1:1.
    // Callers that don't have prevouts (they shouldn't, at consensus
    // time) get the empty-prevouts path: we fall back to witness bytes
    // only (cannot resolve P2MR script type without the prevout).
    const bool have_prevouts = (prevouts.size() == tx.vin.size());

    for (std::size_t i = 0; i < tx.vin.size(); ++i) {
        const auto& input = tx.vin[i];

        // Total witness bytes for this input — the sum across all stack
        // elements. For P2MR this equals the single canonical blob's
        // length. For P2TR key-path it's the 64/65-byte Schnorr sig.
        uint64_t witness_bytes = 0;
        for (const auto& item : input.witness) {
            witness_bytes += item.size();
        }

        // Defaults: each witness byte is 1 VWU, no per-input verify
        // surcharge. Matches the user's clarified spec for
        // non-P2MR inputs.
        uint64_t byte_weight  = 1;
        uint64_t verify_cost  = 0;

        // P2MR surcharge. Uses registry values keyed on the witness
        // blob's scheme_id prefix. We only attempt the lookup when
        // the prevout script is a 34-byte P2MR scriptPubKey; this
        // keeps the formula cheap for the common Taproot case.
        if (have_prevouts &&
            pq::IsP2MRScript(prevouts[i].scriptPubKey) &&
            input.witness.size() == 1 &&
            !input.witness[0].empty()) {
            const uint8_t scheme_id = input.witness[0][0];
            const auto& row = pq::GetSchemeParams(scheme_id);
            // Reserved rows carry zero weights; that would under-price
            // a malformed scheme_id. Refuse to price instead — caller
            // maps this to "reject the tx/block". Validation should
            // have caught this first, but defense-in-depth.
            if (row.state == pq::SchemeState::Reserved) {
                return std::nullopt;
            }
            byte_weight = row.witness_byte_weight;
            verify_cost = row.verify_cost_weight;
        }

        vwu += byte_weight * witness_bytes + verify_cost;
    }

    return vwu;
}

} // namespace dinero::consensus
