/**
 * V7 wallet PQ derivation library tests.
 *
 * Exercises:
 *   - DerivePQSeed against the pinned HKDF vector (cross-arch).
 *   - DerivePQKeypair end-to-end against the pinned pubkey prefix.
 *   - ComputeSingleLeafMerkleRoot matches manual SHA256(0x01 || pubkey).
 *   - Different leaf_index values produce different seeds.
 *   - Zeroization of input buffers doesn't lose determinism (the inputs
 *     are consumed by value, so we can run back-to-back without issue).
 */

#include "wallet/pq_derivation.h"
#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/test_vectors/wallet_hkdf_vectors.h"
#include "crypto/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wpq   = dinero::wallet::pq;
namespace mldsa = dinero::consensus::pq::ml_dsa_65;
namespace tv    = dinero::consensus::pq::test_vectors;

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

} // namespace

int main() {
    std::printf("================================================================\n"
                " Dinero v7 — wallet PQ derivation library tests\n"
                "================================================================\n");

    // Split kHkdfVector_Ikm (64 B) into the BIP-32 priv/chain pair the
    // wallet API expects.
    wpq::Bip32PrivKey   priv{};
    wpq::Bip32ChainCode chain{};
    for (std::size_t i = 0; i < 32; ++i) priv[i]  = tv::kHkdfVector_Ikm[i];
    for (std::size_t i = 0; i < 32; ++i) chain[i] = tv::kHkdfVector_Ikm[32 + i];

    // D1: DerivePQSeed matches the pinned HKDF vector.
    {
        auto pq_seed = wpq::DerivePQSeed(priv, chain, /*leaf_index=*/0);
        bool match = (pq_seed == tv::kHkdfVector_ExpectedPqSeed);
        record(match, "D1: DerivePQSeed(priv,chain,leaf=0) matches pinned pq_seed");
        if (!match) {
            std::printf("       got: ");
            for (auto b : pq_seed) std::printf("%02x", b);
            std::printf("\n       want: ");
            for (auto b : tv::kHkdfVector_ExpectedPqSeed) std::printf("%02x", b);
            std::printf("\n");
        }
    }

    // D2: DerivePQKeypair end-to-end matches the pinned pubkey prefix.
    {
        auto kp = wpq::DerivePQKeypair(priv, chain, /*leaf_index=*/0);
        bool prefix_ok = true;
        for (std::size_t i = 0; i < 32; ++i) {
            if (kp.pubkey()[i] != tv::kHkdfVector_ExpectedPubkeyPrefix[i]) {
                prefix_ok = false;
                break;
            }
        }
        record(prefix_ok, "D2: DerivePQKeypair pubkey prefix matches pinned vector");
    }

    // D3: different leaf_index → different pq_seed.
    {
        auto seed0 = wpq::DerivePQSeed(priv, chain, /*leaf_index=*/0);
        auto seed1 = wpq::DerivePQSeed(priv, chain, /*leaf_index=*/1);
        record(seed0 != seed1, "D3: leaf=0 != leaf=1 pq_seeds");
    }

    // D4: ComputeSingleLeafMerkleRoot matches a manual SHA256(0x01 || pk).
    {
        auto kp = wpq::DerivePQKeypair(priv, chain, /*leaf_index=*/0);

        std::array<uint8_t, 32> expected{};
        {
            uint8_t scheme_id = 0x01;
            dinero::crypto::CSHA256()
                .Write(&scheme_id, 1)
                .Write(kp.pubkey().data(), kp.pubkey().size())
                .Finalize(expected.data());
        }
        auto actual = wpq::ComputeSingleLeafMerkleRoot(0x01, kp.pubkey());
        record(actual == expected,
               "D4: ComputeSingleLeafMerkleRoot == SHA256(0x01 || pubkey)");
    }

    // D5: idempotence — calling DerivePQKeypair twice with same inputs
    // gives byte-identical pubkey (the secret-bytes check is implicit via
    // signing path; here we focus on pubkey for cheap comparability).
    {
        auto kp_a = wpq::DerivePQKeypair(priv, chain, /*leaf_index=*/0);
        auto kp_b = wpq::DerivePQKeypair(priv, chain, /*leaf_index=*/0);
        record(kp_a.pubkey() == kp_b.pubkey(),
               "D5: DerivePQKeypair is idempotent");
        // Sanity: signing works with the derived secret.
        const std::string msg_str = "v7 derived wallet key sign/verify round-trip";
        std::vector<uint8_t> msg(msg_str.begin(), msg_str.end());
        auto sig = mldsa::Sign(msg, kp_a.secret());
        record(mldsa::Verify(msg, sig, kp_a.pubkey()),
               "D5b: signed with derived secret, verified with derived pubkey");
    }

    // D6: merkle root differs for different pubkeys (sanity).
    {
        auto kp0 = wpq::DerivePQKeypair(priv, chain, /*leaf_index=*/0);
        auto kp1 = wpq::DerivePQKeypair(priv, chain, /*leaf_index=*/1);
        auto r0 = wpq::ComputeSingleLeafMerkleRoot(0x01, kp0.pubkey());
        auto r1 = wpq::ComputeSingleLeafMerkleRoot(0x01, kp1.pubkey());
        record(r0 != r1, "D6: merkle roots differ across leaf indices");
    }

    // D7: SecureKeypair scrubs the secret on move.
    //
    // After std::move(src), src.secret() MUST read as all-zero. This
    // guarantees there's only ever one live copy of the 4032-byte secret
    // in memory at a time.
    {
        auto src = wpq::DerivePQKeypair(priv, chain, /*leaf_index=*/0);
        auto live_secret = src.secret();   // snapshot before move for comparison
        dinero::wallet::SecureKeypair dst = std::move(src);

        // dst has the real secret now; verify it matches the snapshot.
        record(dst.secret() == live_secret,
               "D7a: moved-to SecureKeypair holds the original secret bytes");

        // src's secret must be scrubbed (all zero).
        bool src_zero = true;
        for (auto b : src.secret()) {
            if (b != 0x00) { src_zero = false; break; }
        }
        record(src_zero, "D7b: moved-from SecureKeypair secret is scrubbed to zero");

        // And the pubkey is intentionally NOT scrubbed — still readable.
        bool src_pk_nonzero = false;
        for (auto b : src.pubkey()) {
            if (b != 0x00) { src_pk_nonzero = true; break; }
        }
        record(src_pk_nonzero, "D7c: moved-from SecureKeypair pubkey is preserved (public data)");

        // Explicit Wipe() — signing after Wipe() still succeeds trivially
        // because we use a DIFFERENT keypair here to avoid muddying D5b's
        // round-trip semantics.
        auto kp_for_wipe = wpq::DerivePQKeypair(priv, chain, /*leaf_index=*/0);
        kp_for_wipe.Wipe();
        bool wiped_zero = true;
        for (auto b : kp_for_wipe.secret()) {
            if (b != 0x00) { wiped_zero = false; break; }
        }
        record(wiped_zero, "D7d: explicit Wipe() zeros the secret");

        // Adoption from a raw ML-DSA keypair must also scrub the source. This
        // closes the constructor-by-value footgun where the 4032-byte secret
        // survived in the caller's local Keypair after wrapping.
        mldsa::Seed raw_seed{};
        std::memcpy(raw_seed.data(), tv::kHkdfVector_ExpectedPqSeed.data(),
                    raw_seed.size());
        auto raw = mldsa::KeygenFromSeed(raw_seed);
        auto raw_secret = raw.secret;
        dinero::wallet::SecureKeypair adopted(std::move(raw));
        record(adopted.secret() == raw_secret,
               "D7e: adopted SecureKeypair holds the original raw secret bytes");

        bool raw_zero = true;
        for (auto b : raw.secret) {
            if (b != 0x00) { raw_zero = false; break; }
        }
        record(raw_zero, "D7f: raw Keypair source is scrubbed after adoption");
    }

    std::printf("----------------------------------------------------------------\n"
                " RESULT: %d passed, %d failed\n"
                "================================================================\n",
                g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
