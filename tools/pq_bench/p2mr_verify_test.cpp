/**
 * V7 P2MR consensus primitives — integration tests.
 *
 * Constructs a full valid P2MR spend end-to-end:
 *
 *   1. Derive an ML-DSA keypair (seeded, so tests are deterministic).
 *   2. Compute merkle_root = SHA256(scheme_id || pubkey) for single-leaf.
 *   3. Build the 34-byte P2MR scriptPubKey.
 *   4. Sign a test sighash.
 *   5. Pack the witness via SerializeP2MRWitness.
 *   6. VerifyP2MRSpend should return Ok.
 *
 * Then exhaustive negative controls on script shape, witness shape,
 * tampered sig, wrong pubkey, wrong merkle path, depth bounds, etc.
 *
 * Also covers a depth-1 (two-leaf) tree: verify via leaf_index=0 and
 * leaf_index=1 both succeed when siblings are correct.
 */

#include "consensus/pq/p2mr_consensus.h"
#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/scheme_registry.h"
#include "consensus/utxo_entry.h"
#include "primitives/transaction.h"
#include "primitives/amount.h"
#include "wallet/p2mr_address.h"
#include "crypto/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


namespace pq    = dinero::consensus::pq;
namespace mldsa = dinero::consensus::pq::ml_dsa_65;
namespace wallet = dinero::wallet;

namespace {

int g_failed = 0;
int g_passed = 0;

void record(bool cond, const char* tag) {
    if (cond) {
        ++g_passed;
        std::printf("  [PASS] %s\n", tag);
    } else {
        ++g_failed;
        std::printf("  [FAIL] %s\n", tag);
    }
}

std::array<uint8_t, 32> Sha256Concat(const std::array<uint8_t, 32>& a,
                                     const std::array<uint8_t, 32>& b) {
    std::array<uint8_t, 32> out{};
    dinero::crypto::CSHA256()
        .Write(a.data(), a.size())
        .Write(b.data(), b.size())
        .Finalize(out.data());
    return out;
}

mldsa::Seed MakeSeed(uint8_t byte_fill) {
    mldsa::Seed s{};
    for (auto& b : s) b = byte_fill;
    return s;
}

// Helper: the 32-byte SHA-256 sighash used in tests.
std::array<uint8_t, 32> TestSighash(const char* label) {
    std::array<uint8_t, 32> out{};
    dinero::crypto::CSHA256()
        .Write(reinterpret_cast<const uint8_t*>(label), std::strlen(label))
        .Finalize(out.data());
    return out;
}

} // namespace

int main() {
    std::printf("================================================================\n"
                " Dinero v7 — P2MR consensus primitives integration tests\n"
                "================================================================\n");

    // -----------------------------------------------------------------------
    // P1: Script recognition basics
    // -----------------------------------------------------------------------
    {
        std::vector<uint8_t> good(pq::P2MR_SCRIPT_BYTES, 0x00);
        good[0] = pq::P2MR_OP_WITNESS_V3;
        good[1] = pq::P2MR_OP_PUSH32;
        for (std::size_t i = 0; i < 32; ++i) good[2 + i] = static_cast<uint8_t>(0x11 + i);
        record(pq::IsP2MRScript(good),  "P1a: canonical 34-byte script recognized");

        auto root = pq::ExtractP2MRMerkleRoot(good);
        record(root.has_value(),                                   "P1b: merkle root extracted");
        record(root && (*root)[0] == 0x11 && (*root)[31] == (0x11+31), "P1c: extracted root bytes match");

        // Wrong length
        std::vector<uint8_t> too_short(33, 0x53);
        record(!pq::IsP2MRScript(too_short), "P1d: 33-byte script rejected");
        std::vector<uint8_t> too_long(35, 0x53);
        record(!pq::IsP2MRScript(too_long),  "P1e: 35-byte script rejected");

        // Wrong opcode bytes
        std::vector<uint8_t> wrong_op = good;
        wrong_op[0] = 0x51;  // OP_1 (would be Taproot if the script were v1)
        record(!pq::IsP2MRScript(wrong_op), "P1f: leading byte != 0x53 rejected");
        wrong_op = good;
        wrong_op[1] = 0x21;
        record(!pq::IsP2MRScript(wrong_op), "P1g: second byte != 0x20 rejected");
    }

    // -----------------------------------------------------------------------
    // Build a depth-0 (single-leaf) spend end-to-end.
    // -----------------------------------------------------------------------
    auto seed_a = MakeSeed(0xA3);
    auto kp_a   = mldsa::KeygenFromSeed(seed_a);

    // Single-leaf merkle root
    std::array<uint8_t, 32> leaf_a = pq::ComputeP2MRLeafHash(
        pq::SCHEME_ID_ML_DSA_65, kp_a.pubkey.data(), kp_a.pubkey.size());
    // For depth=0 the root equals the leaf hash.
    std::array<uint8_t, 32> root_a = leaf_a;

    // Build P2MR scriptPubKey
    std::vector<uint8_t> spk_a(pq::P2MR_SCRIPT_BYTES, 0x00);
    spk_a[0] = pq::P2MR_OP_WITNESS_V3;
    spk_a[1] = pq::P2MR_OP_PUSH32;
    std::memcpy(spk_a.data() + 2, root_a.data(), 32);

    // Sighash + signature
    auto sighash = TestSighash("v7 p2mr spend canary");
    std::vector<uint8_t> msg(sighash.begin(), sighash.end());
    auto sig_a = mldsa::Sign(msg, kp_a.secret);

    // Canonical witness
    pq::P2MRWitness w{};
    w.scheme_id       = pq::SCHEME_ID_ML_DSA_65;
    w.pubkey_bytes    = std::vector<uint8_t>(kp_a.pubkey.begin(), kp_a.pubkey.end());
    w.signature_bytes = std::vector<uint8_t>(sig_a.begin(),       sig_a.end());
    w.merkle_depth    = 0;
    w.sibling_hashes  = {};
    w.leaf_index      = 0;

    const auto witness_bytes = pq::SerializeP2MRWitness(w);
    record(!witness_bytes.empty(),
           "P2a: SerializeP2MRWitness produces non-empty bytes for valid input");

    // Round-trip decode
    {
        pq::P2MRWitness decoded{};
        auto rc = pq::DeserializeP2MRWitness(witness_bytes, &decoded);
        record(rc == pq::P2MRWitnessDecodeError::Ok,
               "P2b: witness decode returns Ok");
        record(decoded.scheme_id       == w.scheme_id
               && decoded.pubkey_bytes    == w.pubkey_bytes
               && decoded.signature_bytes == w.signature_bytes
               && decoded.merkle_depth    == w.merkle_depth
               && decoded.sibling_hashes  == w.sibling_hashes
               && decoded.leaf_index      == w.leaf_index,
               "P2c: witness decodes to byte-identical fields");
    }

    // -----------------------------------------------------------------------
    // P3: End-to-end VerifyP2MRSpend on the happy path
    // -----------------------------------------------------------------------
    {
        auto rc = pq::VerifyP2MRSpend(spk_a, witness_bytes, sighash, /*height=*/0);
        record(rc == pq::P2MRVerifyError::Ok,
               "P3: VerifyP2MRSpend Ok on depth-0 happy path");
    }

    // -----------------------------------------------------------------------
    // P4: Negative controls on VerifyP2MRSpend
    // -----------------------------------------------------------------------
    {
        // Tampered sighash — signature still valid vs ORIGINAL sighash so
        // verify must fail.
        auto wrong_sighash = sighash;
        wrong_sighash[0] ^= 0x01;
        auto rc = pq::VerifyP2MRSpend(spk_a, witness_bytes, wrong_sighash, 0);
        record(rc == pq::P2MRVerifyError::SignatureInvalid,
               "P4a: wrong sighash → SignatureInvalid");

        // Tampered pubkey in witness (bit flip late in the bytes)
        auto bad_pk_w = w;
        bad_pk_w.pubkey_bytes[100] ^= 0x01;
        auto bad_pk_bytes = pq::SerializeP2MRWitness(bad_pk_w);
        rc = pq::VerifyP2MRSpend(spk_a, bad_pk_bytes, sighash, 0);
        // Tampered pubkey changes the leaf hash → merkle mismatch, not
        // SignatureInvalid. Either bad-shape or SignatureInvalid or mismatch
        // is acceptable as "rejected".
        record(rc != pq::P2MRVerifyError::Ok,
               "P4b: tampered pubkey in witness → rejected");

        // Tampered signature bytes
        auto bad_sig_w = w;
        bad_sig_w.signature_bytes[50] ^= 0x01;
        auto bad_sig_bytes = pq::SerializeP2MRWitness(bad_sig_w);
        rc = pq::VerifyP2MRSpend(spk_a, bad_sig_bytes, sighash, 0);
        record(rc == pq::P2MRVerifyError::SignatureInvalid,
               "P4c: tampered signature → SignatureInvalid");

        // Wrong scriptPubKey commitment (random root)
        std::vector<uint8_t> wrong_spk = spk_a;
        wrong_spk[10] ^= 0xFF;
        rc = pq::VerifyP2MRSpend(wrong_spk, witness_bytes, sighash, 0);
        record(rc == pq::P2MRVerifyError::MerklePathMismatch,
               "P4d: wrong commitment → MerklePathMismatch");

        // Malformed script (not 34 bytes)
        std::vector<uint8_t> too_short_spk(30, 0x53);
        rc = pq::VerifyP2MRSpend(too_short_spk, witness_bytes, sighash, 0);
        record(rc == pq::P2MRVerifyError::BadScriptShape,
               "P4e: <34-byte script → BadScriptShape");

        // Truncated witness
        std::vector<uint8_t> trunc(witness_bytes.begin(), witness_bytes.begin() + 100);
        rc = pq::VerifyP2MRSpend(spk_a, trunc, sighash, 0);
        record(rc == pq::P2MRVerifyError::WitnessDecodeFailed,
               "P4f: truncated witness → WitnessDecodeFailed");

        // Unknown scheme_id (0xFF — Reserved in the registry)
        auto bad_scheme_w = w;
        bad_scheme_w.scheme_id = 0xFF;
        auto bad_scheme_bytes = pq::SerializeP2MRWitness(bad_scheme_w);
        rc = pq::VerifyP2MRSpend(spk_a, bad_scheme_bytes, sighash, 0);
        record(rc != pq::P2MRVerifyError::Ok,
               "P4g: scheme_id=0xFF (Reserved) → rejected");

        // DarkReserved scheme_id (0x02 — FALCON, not yet activated)
        auto dark_scheme_w = w;
        dark_scheme_w.scheme_id = pq::SCHEME_ID_FALCON_512;
        auto dark_scheme_bytes = pq::SerializeP2MRWitness(dark_scheme_w);
        rc = pq::VerifyP2MRSpend(spk_a, dark_scheme_bytes, sighash, 0);
        record(rc != pq::P2MRVerifyError::Ok,
               "P4h: scheme_id=0x02 (DarkReserved FALCON) → rejected");
    }

    // -----------------------------------------------------------------------
    // P5: Merkle path — depth-1 tree (two leaves)
    //
    // Leaf A (our kp_a) and a synthetic leaf B. Root = SHA256(LeafA||LeafB).
    // We verify spends from both positions (leaf_index=0 and leaf_index=1).
    // -----------------------------------------------------------------------
    {
        auto seed_b = MakeSeed(0xB7);
        auto kp_b   = mldsa::KeygenFromSeed(seed_b);
        std::array<uint8_t, 32> leaf_b = pq::ComputeP2MRLeafHash(
            pq::SCHEME_ID_ML_DSA_65, kp_b.pubkey.data(), kp_b.pubkey.size());

        // Canonical depth-1 tree: leaf A is at index 0 (left), leaf B at index 1 (right).
        std::array<uint8_t, 32> root_d1 = Sha256Concat(leaf_a, leaf_b);

        std::vector<uint8_t> spk_d1(pq::P2MR_SCRIPT_BYTES, 0x00);
        spk_d1[0] = pq::P2MR_OP_WITNESS_V3;
        spk_d1[1] = pq::P2MR_OP_PUSH32;
        std::memcpy(spk_d1.data() + 2, root_d1.data(), 32);

        auto sh1 = TestSighash("v7 p2mr depth-1 spend");

        // Spend from leaf_index=0 (A is left child; B is sibling).
        {
            std::vector<uint8_t> msg1(sh1.begin(), sh1.end());
            auto sig = mldsa::Sign(msg1, kp_a.secret);

            pq::P2MRWitness wA{};
            wA.scheme_id       = pq::SCHEME_ID_ML_DSA_65;
            wA.pubkey_bytes    = std::vector<uint8_t>(kp_a.pubkey.begin(), kp_a.pubkey.end());
            wA.signature_bytes = std::vector<uint8_t>(sig.begin(), sig.end());
            wA.merkle_depth    = 1;
            wA.sibling_hashes  = { leaf_b };
            wA.leaf_index      = 0;

            auto bytesA = pq::SerializeP2MRWitness(wA);
            auto rc = pq::VerifyP2MRSpend(spk_d1, bytesA, sh1, 0);
            record(rc == pq::P2MRVerifyError::Ok,
                   "P5a: depth-1 leaf_index=0 spend Ok");
        }
        // Spend from leaf_index=1 (B is right child; A is sibling).
        {
            std::vector<uint8_t> msg1(sh1.begin(), sh1.end());
            auto sig = mldsa::Sign(msg1, kp_b.secret);

            pq::P2MRWitness wB{};
            wB.scheme_id       = pq::SCHEME_ID_ML_DSA_65;
            wB.pubkey_bytes    = std::vector<uint8_t>(kp_b.pubkey.begin(), kp_b.pubkey.end());
            wB.signature_bytes = std::vector<uint8_t>(sig.begin(), sig.end());
            wB.merkle_depth    = 1;
            wB.sibling_hashes  = { leaf_a };
            wB.leaf_index      = 1;

            auto bytesB = pq::SerializeP2MRWitness(wB);
            auto rc = pq::VerifyP2MRSpend(spk_d1, bytesB, sh1, 0);
            record(rc == pq::P2MRVerifyError::Ok,
                   "P5b: depth-1 leaf_index=1 spend Ok");
        }
        // Spend with wrong leaf_index (claim index 1 but with leaf A's key
        // and siblings that would belong to index 0). The merkle path
        // recomputes to the wrong root; reject.
        {
            std::vector<uint8_t> msg1(sh1.begin(), sh1.end());
            auto sig = mldsa::Sign(msg1, kp_a.secret);

            pq::P2MRWitness wMix{};
            wMix.scheme_id       = pq::SCHEME_ID_ML_DSA_65;
            wMix.pubkey_bytes    = std::vector<uint8_t>(kp_a.pubkey.begin(), kp_a.pubkey.end());
            wMix.signature_bytes = std::vector<uint8_t>(sig.begin(), sig.end());
            wMix.merkle_depth    = 1;
            wMix.sibling_hashes  = { leaf_b };
            wMix.leaf_index      = 1;   // wrong — A is at index 0

            auto mixBytes = pq::SerializeP2MRWitness(wMix);
            auto rc = pq::VerifyP2MRSpend(spk_d1, mixBytes, sh1, 0);
            record(rc == pq::P2MRVerifyError::MerklePathMismatch,
                   "P5c: wrong leaf_index → MerklePathMismatch");
        }
    }

    // -----------------------------------------------------------------------
    // P6: Merkle-depth bounds
    // -----------------------------------------------------------------------
    {
        // depth=9 (one over cap) rejected at serialize time
        pq::P2MRWitness too_deep{};
        too_deep.scheme_id       = pq::SCHEME_ID_ML_DSA_65;
        too_deep.pubkey_bytes    = std::vector<uint8_t>(kp_a.pubkey.begin(), kp_a.pubkey.end());
        too_deep.signature_bytes = std::vector<uint8_t>(sig_a.begin(),       sig_a.end());
        too_deep.merkle_depth    = 9;
        too_deep.sibling_hashes.resize(9);
        too_deep.leaf_index      = 0;
        auto bytes = pq::SerializeP2MRWitness(too_deep);
        record(bytes.empty(),
               "P6a: SerializeP2MRWitness refuses depth > 8");

        // Build a manually-packed depth=9 witness and feed to the decoder.
        // We construct the bytes by hand with depth byte = 9.
        {
            std::vector<uint8_t> forged;
            forged.push_back(pq::SCHEME_ID_ML_DSA_65);
            // pubkey len (1952) as varint
            forged.push_back(0xfd);
            forged.push_back(0xa0); forged.push_back(0x07);  // 1952 LE
            forged.insert(forged.end(), kp_a.pubkey.begin(), kp_a.pubkey.end());
            // sig len (3309)
            forged.push_back(0xfd);
            forged.push_back(0xed); forged.push_back(0x0c);  // 3309 LE
            forged.insert(forged.end(), sig_a.begin(), sig_a.end());
            // merkle_depth = 9
            forged.push_back(9);
            // 9 * 32 bytes of siblings (dummy)
            for (int i = 0; i < 9 * 32; ++i) forged.push_back(0);
            // leaf_index = 0
            forged.push_back(0);

            pq::P2MRWitness dec{};
            auto rc = pq::DeserializeP2MRWitness(forged, &dec);
            record(rc == pq::P2MRWitnessDecodeError::MerkleDepthTooDeep,
                   "P6b: decoder rejects depth=9 as MerkleDepthTooDeep");
        }
    }

    // -----------------------------------------------------------------------
    // P7: Witness-codec edge cases
    // -----------------------------------------------------------------------
    {
        // Trailing bytes
        auto bytes = witness_bytes;
        bytes.push_back(0x00);
        pq::P2MRWitness dec{};
        auto rc = pq::DeserializeP2MRWitness(bytes, &dec);
        record(rc == pq::P2MRWitnessDecodeError::TrailingBytes,
               "P7a: trailing byte → TrailingBytes");

        // Zero-length signature
        auto w2 = w;
        w2.signature_bytes.clear();
        auto b2 = pq::SerializeP2MRWitness(w2);
        // Serializer accepts (we let the length field say 0) — but decoder
        // should reject as SignatureLenInvalid per the contract.
        if (!b2.empty()) {
            pq::P2MRWitness dec2{};
            auto rc2 = pq::DeserializeP2MRWitness(b2, &dec2);
            record(rc2 == pq::P2MRWitnessDecodeError::SignatureLenInvalid,
                   "P7b: zero-length signature → SignatureLenInvalid");
        }

        // Zero-length pubkey
        auto w3 = w;
        w3.pubkey_bytes.clear();
        auto b3 = pq::SerializeP2MRWitness(w3);
        if (!b3.empty()) {
            pq::P2MRWitness dec3{};
            auto rc3 = pq::DeserializeP2MRWitness(b3, &dec3);
            record(rc3 == pq::P2MRWitnessDecodeError::PubkeyLenInvalid,
                   "P7c: zero-length pubkey → PubkeyLenInvalid");
        }
    }

    // -----------------------------------------------------------------------
    // P8: Wallet codec round-trip vs. scriptPubKey
    //
    // wallet::BuildP2MRScriptPubKey (Phase 4a) should produce the exact
    // 34-byte script that consensus IsP2MRScript accepts and
    // ExtractP2MRMerkleRoot round-trips to.
    // -----------------------------------------------------------------------
    {
        auto spk_from_wallet = wallet::BuildP2MRScriptPubKey({
            root_a[0],  root_a[1],  root_a[2],  root_a[3],
            root_a[4],  root_a[5],  root_a[6],  root_a[7],
            root_a[8],  root_a[9],  root_a[10], root_a[11],
            root_a[12], root_a[13], root_a[14], root_a[15],
            root_a[16], root_a[17], root_a[18], root_a[19],
            root_a[20], root_a[21], root_a[22], root_a[23],
            root_a[24], root_a[25], root_a[26], root_a[27],
            root_a[28], root_a[29], root_a[30], root_a[31],
        });
        record(spk_from_wallet == spk_a,
               "P8: wallet::BuildP2MRScriptPubKey == consensus-side expected");
    }

    // -----------------------------------------------------------------------
    // P9: Canonicalization (Phase 6 Commit 5)
    //
    // Rule: one valid P2MR spend has exactly one valid byte encoding.
    // Every shape a non-canonical signer could emit must be rejected.
    // -----------------------------------------------------------------------
    {
        // P9a: short-but-nonzero pubkey (fixed-length scheme rejects != 1952).
        auto w_short_pk = w;
        w_short_pk.pubkey_bytes.resize(w.pubkey_bytes.size() - 1);  // 1951 bytes
        auto b_short_pk = pq::SerializeP2MRWitness(w_short_pk);
        if (!b_short_pk.empty()) {
            pq::P2MRWitness dec{};
            auto rc = pq::DeserializeP2MRWitness(b_short_pk, &dec);
            record(rc == pq::P2MRWitnessDecodeError::PubkeyLenInvalid,
                   "P9a: pubkey one byte short → PubkeyLenInvalid");
        } else {
            record(false, "P9a: serializer refused short pubkey (expected to emit and let decoder reject)");
        }

        // P9b: long pubkey (fixed+1 bytes).
        auto w_long_pk = w;
        w_long_pk.pubkey_bytes.push_back(0x00);  // 1953 bytes
        auto b_long_pk = pq::SerializeP2MRWitness(w_long_pk);
        if (!b_long_pk.empty()) {
            pq::P2MRWitness dec{};
            auto rc = pq::DeserializeP2MRWitness(b_long_pk, &dec);
            record(rc == pq::P2MRWitnessDecodeError::PubkeyLenInvalid,
                   "P9b: pubkey one byte long → PubkeyLenInvalid");
        }

        // P9c: signature one byte short.
        auto w_short_sig = w;
        w_short_sig.signature_bytes.resize(w.signature_bytes.size() - 1);  // 3308 bytes
        auto b_short_sig = pq::SerializeP2MRWitness(w_short_sig);
        if (!b_short_sig.empty()) {
            pq::P2MRWitness dec{};
            auto rc = pq::DeserializeP2MRWitness(b_short_sig, &dec);
            record(rc == pq::P2MRWitnessDecodeError::SignatureLenInvalid,
                   "P9c: signature one byte short → SignatureLenInvalid");
        }

        // P9d: non-canonical CompactSize on pubkey_len.
        //
        // Canonical encoding of 1952 is {0xfd 0xa0 0x07}. We rewrite it
        // as the 5-byte form {0xfe 0xa0 0x07 0x00 0x00} — same value,
        // non-minimal. The length-equality check accepts (1952 == fixed),
        // the byte-budget checks hold; only the reserialize-equals
        // canonicalization gate catches this. Hard-positive that the
        // new gate is actually live.
        auto nc = witness_bytes;
        // Layout: [scheme_id=1][0xfd a0 07][pubkey 1952]...
        // Offset 1..3 is the pubkey CompactSize (3 bytes). Splice in
        // the 5-byte form at that position.
        nc.erase(nc.begin() + 1, nc.begin() + 4);                    // drop {fd a0 07}
        const uint8_t pk_5byte[5] = {0xfe, 0xa0, 0x07, 0x00, 0x00};  // 1952 in 5-byte form
        nc.insert(nc.begin() + 1, pk_5byte, pk_5byte + 5);
        {
            pq::P2MRWitness dec{};
            auto rc = pq::DeserializeP2MRWitness(nc, &dec);
            record(rc == pq::P2MRWitnessDecodeError::NonCanonical,
                   "P9d: non-minimal CompactSize on pubkey_len → NonCanonical");
        }

        // P9e: reserialize round-trip determinism.
        //
        // Serializer is pure; serialize(w) at call N must equal serialize(w)
        // at call N+1 for the same w. Cheap safety net against future
        // drift (e.g. someone adding a randomized field, a std::map
        // iteration order dependency, etc).
        auto b1 = pq::SerializeP2MRWitness(w);
        auto b2 = pq::SerializeP2MRWitness(w);
        record(b1 == b2 && b1 == witness_bytes,
               "P9e: SerializeP2MRWitness is bit-deterministic on identical input");

        // P9f: VerifyP2MRSpend refuses a non-canonical witness even when
        // the signature + root + sighash would otherwise be accepted.
        // This is the consensus-side backstop: the canonical gate is
        // reachable through the dispatcher, not just the unit-test
        // layer.
        auto verr = pq::VerifyP2MRSpend(spk_a, nc, sighash, /*height=*/0);
        record(verr == pq::P2MRVerifyError::WitnessDecodeFailed,
               "P9f: end-to-end VerifyP2MRSpend rejects non-canonical witness");
    }

    // -----------------------------------------------------------------------
    // P10: ComputeVWU (Phase 8 Commit 1)
    //
    // Rule: VWU(tx) = stripped_size + Σ_inputs (byte_weight * witness_bytes +
    //                                           verify_cost_weight).
    // Non-P2MR inputs: implicit byte_weight=1, verify_cost_weight=0.
    // P2MR inputs:     byte_weight/verify_cost from scheme registry.
    // -----------------------------------------------------------------------
    {
        using dinero::Transaction;
        using dinero::TxInput;
        using dinero::TxOutput;
        using dinero::AmountUna;
        using dinero::consensus::UTXOEntry;

        auto make_tx_one_input = [](std::vector<uint8_t> witness_blob,
                                    std::vector<uint8_t> out_spk,
                                    uint64_t out_value_sat) -> Transaction {
            Transaction tx;
            tx.version = 2;
            tx.lockTime = 0;
            TxInput in;
            in.prevout.txid     = dinero::TxId(dinero::uint256());
            in.prevout.vout     = 0;
            in.sequence         = 0xfffffffe;
            if (!witness_blob.empty()) {
                in.witness.push_back(std::move(witness_blob));
            }
            tx.vin.push_back(std::move(in));
            TxOutput out;
            out.value        = AmountUna::Una(out_value_sat);
            out.scriptPubKey = std::move(out_spk);
            tx.vout.push_back(std::move(out));
            return tx;
        };

        const auto& row_mldsa = pq::GetSchemeParams(pq::SCHEME_ID_ML_DSA_65);

        // P10a: P2MR-input tx — VWU = stripped + 1*W + 25 for ML-DSA.
        {
            Transaction tx = make_tx_one_input(witness_bytes,
                                               /*out_spk=*/{0x51, 0x20,
                                                            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                                            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                                            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                                            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f},
                                               /*out_value=*/42);
            UTXOEntry prev;
            prev.value        = AmountUna::Una(100);
            prev.scriptPubKey = spk_a;   // P2MR (OP_3 || 32-byte root)
            prev.height       = 0;
            prev.isCoinbase   = false;
            std::vector<UTXOEntry> prevouts{prev};

            auto vwu_opt = dinero::consensus::ComputeVWU(tx, prevouts);
            const uint64_t stripped      = tx.GetBaseSize();
            const uint64_t witness_len   = witness_bytes.size();
            const uint64_t expected_vwu  = stripped
                                         + uint64_t{row_mldsa.witness_byte_weight} * witness_len
                                         + uint64_t{row_mldsa.verify_cost_weight};

            record(vwu_opt.has_value(),             "P10a: ComputeVWU returns Ok for P2MR tx");
            if (vwu_opt) {
                record(*vwu_opt == expected_vwu,    "P10a.1: P2MR VWU = stripped + 1*W + 25");
            }
        }

        // P10b: non-P2MR (synthetic Taproot-like) input — implicit
        // byte_weight=1, verify_cost=0. VWU = stripped + witness_bytes.
        {
            std::vector<uint8_t> taproot_sig(64, 0xaa);   // fake 64-byte Schnorr sig
            Transaction tx = make_tx_one_input(taproot_sig,
                                               /*out_spk=*/{0x51, 0x20,
                                                            0, 0, 0, 0, 0, 0, 0, 0,
                                                            0, 0, 0, 0, 0, 0, 0, 0,
                                                            0, 0, 0, 0, 0, 0, 0, 0,
                                                            0, 0, 0, 0, 0, 0, 0, 0},
                                               /*out_value=*/1);
            UTXOEntry prev;
            prev.value        = AmountUna::Una(100);
            // P2TR scriptPubKey: OP_1 (0x51) + PUSH 32 + 32 bytes. NOT P2MR.
            prev.scriptPubKey = {0x51, 0x20,
                                 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
                                 17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
            prev.height       = 0;
            prev.isCoinbase   = false;
            std::vector<UTXOEntry> prevouts{prev};

            auto vwu_opt = dinero::consensus::ComputeVWU(tx, prevouts);
            const uint64_t stripped     = tx.GetBaseSize();
            const uint64_t expected_vwu = stripped + 64;

            record(vwu_opt.has_value(),             "P10b: ComputeVWU returns Ok for non-P2MR tx");
            if (vwu_opt) {
                record(*vwu_opt == expected_vwu,    "P10b.1: Non-P2MR VWU = stripped + witness_bytes");
            }
        }

        // P10c: coinbase — zero inputs charged, VWU = stripped_size only.
        {
            Transaction tx;
            tx.version = 2;
            tx.lockTime = 0;
            TxInput cb;
            cb.prevout.txid = dinero::TxId(dinero::uint256());  // null prevout
            cb.prevout.vout = 0xffffffff;                       // coinbase marker
            cb.sequence     = 0xffffffff;
            cb.scriptSig    = {0x01, 0x01};  // BIP34 height placeholder
            tx.vin.push_back(std::move(cb));
            TxOutput out;
            out.value = AmountUna::Una(5000000000ULL);
            out.scriptPubKey = {0x51, 0x20,
                                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
            tx.vout.push_back(std::move(out));

            auto vwu_opt = dinero::consensus::ComputeVWU(tx, /*prevouts=*/{});
            const uint64_t expected_vwu = tx.GetBaseSize();

            record(vwu_opt.has_value(),             "P10c: ComputeVWU returns Ok for coinbase");
            if (vwu_opt) {
                record(*vwu_opt == expected_vwu,    "P10c.1: Coinbase VWU = stripped_size (no input charge)");
            }
        }

        // P10d: P2MR under-priced vs BIP141 vsize — demonstrates why the
        // metric change matters. Under BIP141, the witness contributes
        // witness_bytes/4 to vsize (the 4× discount); under VWU, it
        // contributes witness_bytes * 1 + 25 verify. For ML-DSA-65's
        // ~5270-byte witness, VWU is roughly 4× BIP141 vsize for the
        // same tx — exactly the spam-resistance gain we want.
        {
            Transaction tx = make_tx_one_input(witness_bytes,
                                               /*out_spk=*/{0x51, 0x20,
                                                            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                                            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                                               /*out_value=*/1);
            UTXOEntry prev;
            prev.value        = AmountUna::Una(100);
            prev.scriptPubKey = spk_a;
            prev.height       = 0;
            prev.isCoinbase   = false;
            std::vector<UTXOEntry> prevouts{prev};

            auto vwu_opt  = dinero::consensus::ComputeVWU(tx, prevouts);
            const uint64_t vsize = tx.GetVirtualSize();
            record(vwu_opt.has_value() && *vwu_opt > uint64_t{3} * vsize,
                   "P10d: P2MR VWU > 3× BIP141 vsize (closes the witness discount)");
        }
    }

    std::printf("----------------------------------------------------------------\n"
                " RESULT: %d passed, %d failed\n"
                "================================================================\n",
                g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
