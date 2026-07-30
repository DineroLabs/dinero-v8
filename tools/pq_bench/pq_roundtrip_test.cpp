/**
 * ML-DSA-65 + PQSchemeRegistry round-trip tests.
 *
 * Standalone test executable. No GoogleTest / ctest dependency on purpose —
 * this runs before the main consensus build is wired and needs to be
 * maximally portable. Returns 0 on success, non-zero on failure.
 *
 * Test plan:
 *   T1. Keygen produces pubkey and secret of the expected fixed lengths.
 *   T2. Sign(msg) + Verify(msg) round-trip returns true.
 *   T3. Verify(tampered_msg) returns false.
 *   T4. Verify(tampered_sig) returns false.
 *   T5. Verify(wrong_pubkey) returns false.
 *   T6. Verify with mismatched sig_len / pubkey_len is rejected by the length
 *       gates in the wrapper (before PQClean is even called).
 *   T7. Registry: GetSchemeParams(0x01) == ML-DSA-65 row, state == Accept,
 *       activation_height == 0.
 *   T8. Registry: GetSchemeParams(0x02) / 0x03 are DarkReserved.
 *   T9. Registry: GetSchemeParams(0x04..0xFF) resolve to Reserved sentinel.
 *   T10. Registry: IsSchemeAcceptedAtHeight(0x01, 0) == true.
 *   T11. Registry: IsSchemeAcceptedAtHeight(0x02, 0) == false (DarkReserved).
 *   T12. Registry: IsSchemeAcceptedAtHeight(0xFF, 1'000'000) == false.
 *
 *   T13. KeygenFromSeed(seed) produces byte-identical output on two calls
 *        with the same seed.
 *   T14. KeygenFromSeed(seedA) differs from KeygenFromSeed(seedB) for
 *        seedA != seedB.
 *   T15. Keypair from KeygenFromSeed round-trips sign + verify.
 *   T16. After KeygenFromSeed returns, a plain Keygen() produces a DIFFERENT
 *        keypair (platform randomness restored; seeded mode was scoped).
 *   T17. Known-answer pin: keygen from a fixed seed produces a pubkey whose
 *        SHA-256 equals the value recorded here. This is the cross-arch
 *        reproducibility anchor — if this changes, something in the PQ
 *        stack has drifted and wallet derivation is no longer portable.
 */

#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/scheme_registry.h"
#include "consensus/pq/test_vectors/ml_dsa_65_keygen_vectors.h"
#include "consensus/pq/test_vectors/ml_dsa_65_sig_canary.h"
#include "consensus/pq/test_vectors/wallet_hkdf_vectors.h"

// HMAC-SHA256 via OpenSSL for the HKDF vector test. We use the existing
// vendored OpenSSL so this test is self-contained. In the wallet RPC code
// path HKDF will use the same primitive.
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace mldsa = dinero::consensus::pq::ml_dsa_65;
namespace pq    = dinero::consensus::pq;

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

std::vector<uint8_t> make_message(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

int main() {
    std::printf("================================================================\n"
                " Dinero v7 — PQ primitives round-trip test\n"
                "================================================================\n");

    // T1: keygen size invariants
    auto kp = mldsa::Keygen();
    record(kp.pubkey.size() == mldsa::PUBKEY_BYTES,    "T1: pubkey size");
    record(kp.secret.size() == mldsa::SECRETKEY_BYTES, "T1: secret size");

    // T2: sign + verify happy path
    const auto msg = make_message("dinero v7 p2mr round-trip test vector 1");
    auto sig = mldsa::Sign(msg, kp.secret);
    record(sig.size() == mldsa::SIGNATURE_BYTES, "T2: signature size");
    record(mldsa::Verify(msg, sig, kp.pubkey),   "T2: verify happy path");

    // T3: tampered message fails verification
    auto tampered_msg = msg;
    tampered_msg[0] ^= 0x01;
    record(!mldsa::Verify(tampered_msg, sig, kp.pubkey),
           "T3: tampered message rejected");

    // T4: tampered signature fails verification
    auto tampered_sig = sig;
    tampered_sig[100] ^= 0x01; // flip a byte mid-signature
    record(!mldsa::Verify(msg, tampered_sig, kp.pubkey),
           "T4: tampered signature rejected");

    // T5: wrong pubkey fails
    auto kp2 = mldsa::Keygen();
    record(!mldsa::Verify(msg, sig, kp2.pubkey),
           "T5: wrong pubkey rejected");

    // T6: wrapper-level length gates (before PQClean is even called)
    {
        // Wrong sig length
        const bool ok_short_sig = mldsa::Verify(
            msg.data(), msg.size(),
            sig.data(), sig.size() - 1,
            kp.pubkey.data(), kp.pubkey.size());
        record(!ok_short_sig, "T6a: short-sig rejected by length gate");

        // Wrong pubkey length
        const bool ok_short_pk = mldsa::Verify(
            msg.data(), msg.size(),
            sig.data(), sig.size(),
            kp.pubkey.data(), kp.pubkey.size() - 1);
        record(!ok_short_pk, "T6b: short-pubkey rejected by length gate");
    }

    // T7-T12: registry
    {
        const auto& ml = pq::GetSchemeParams(pq::SCHEME_ID_ML_DSA_65);
        record(ml.state == pq::SchemeState::Accept,       "T7a: ML-DSA-65 state == Accept");
        record(ml.activation_height == 0,                 "T7b: ML-DSA-65 activation_height == 0");
        record(ml.pubkey_bytes_max == mldsa::PUBKEY_BYTES,"T7c: ML-DSA-65 pubkey_bytes_max");
        record(ml.signature_bytes_max == mldsa::SIGNATURE_BYTES,
                                                           "T7d: ML-DSA-65 signature_bytes_max");
        record(ml.witness_byte_weight == 1,               "T7e: ML-DSA-65 witness_byte_weight == 1");
        record(ml.verify_cost_weight == 25,               "T7f: ML-DSA-65 verify_cost_weight == 25");

        const auto& falcon = pq::GetSchemeParams(pq::SCHEME_ID_FALCON_512);
        record(falcon.state == pq::SchemeState::DarkReserved,
                                                           "T8a: FALCON-512 state == DarkReserved");
        record(pq::IsSchemeDarkReserved(pq::SCHEME_ID_FALCON_512),
                                                           "T8b: IsSchemeDarkReserved(FALCON) == true");

        const auto& sphincs = pq::GetSchemeParams(pq::SCHEME_ID_SPHINCS_128);
        record(sphincs.state == pq::SchemeState::DarkReserved,
                                                           "T8c: SPHINCS+ state == DarkReserved");

        // T9: all unassigned scheme_ids resolve to Reserved
        int reserved_count = 0;
        int total_unassigned = 0;
        for (int i = 0; i <= 0xFF; ++i) {
            const uint8_t sid = static_cast<uint8_t>(i);
            if (sid == pq::SCHEME_ID_ML_DSA_65 ||
                sid == pq::SCHEME_ID_FALCON_512 ||
                sid == pq::SCHEME_ID_SPHINCS_128) {
                continue;
            }
            ++total_unassigned;
            if (pq::GetSchemeParams(sid).state == pq::SchemeState::Reserved) {
                ++reserved_count;
            }
        }
        record(reserved_count == total_unassigned,
               "T9: all unassigned scheme_ids resolve to Reserved");

        // T10: ML-DSA accepted at genesis
        record(pq::IsSchemeAcceptedAtHeight(pq::SCHEME_ID_ML_DSA_65, 0),
               "T10: ML-DSA accepted at height 0");

        // T11: FALCON rejected (DarkReserved) even at far-future height
        record(!pq::IsSchemeAcceptedAtHeight(pq::SCHEME_ID_FALCON_512, 1'000'000u),
               "T11: FALCON (DarkReserved) rejected even at far-future height");

        // T12: reserved byte 0xFF rejected at any height
        record(!pq::IsSchemeAcceptedAtHeight(0xFFu, 1'000'000u),
               "T12: reserved 0xFF rejected at any height");
    }

    // -----------------------------------------------------------------------
    // Phase 4b: KeygenFromSeed determinism tests
    // -----------------------------------------------------------------------
    {
        // A fixed 32-byte seed — same bytes used on every host. This is the
        // cross-arch reproducibility anchor.
        mldsa::Seed seed_a{};
        for (std::size_t i = 0; i < seed_a.size(); ++i) {
            seed_a[i] = static_cast<uint8_t>(0xA0 + (i & 0x0F));
        }
        mldsa::Seed seed_b = seed_a;
        seed_b[0] ^= 0x01;  // flip one bit → different seed

        // T13: same seed → same keypair bytes (on this host).
        auto kp1 = mldsa::KeygenFromSeed(seed_a);
        auto kp2 = mldsa::KeygenFromSeed(seed_a);
        record(kp1.pubkey == kp2.pubkey, "T13a: same seed → same pubkey");
        record(kp1.secret == kp2.secret, "T13b: same seed → same secret");

        // T14: different seed → different keypair.
        auto kp3 = mldsa::KeygenFromSeed(seed_b);
        record(kp1.pubkey != kp3.pubkey, "T14a: different seed → different pubkey");
        record(kp1.secret != kp3.secret, "T14b: different seed → different secret");

        // T15: seeded keypair still verifies a signed message.
        const auto msg_seed = make_message("seeded keygen sign/verify round-trip");
        auto sig_seed = mldsa::Sign(msg_seed, kp1.secret);
        record(mldsa::Verify(msg_seed, sig_seed, kp1.pubkey),
               "T15: seeded keypair signs + verifies");

        // T16: after KeygenFromSeed returns, plain Keygen() is non-deterministic
        // — platform randomness has been restored. Two plain Keygen()s in a
        // row must differ.
        auto kr1 = mldsa::Keygen();
        auto kr2 = mldsa::Keygen();
        record(kr1.pubkey != kr2.pubkey,
               "T16a: two plain Keygen() calls produce different pubkeys (platform RNG restored)");
        // And neither matches the seeded pubkey from T13.
        record(kr1.pubkey != kp1.pubkey,
               "T16b: plain Keygen() != seeded pubkey (no mode leak)");

    }

    // -----------------------------------------------------------------------
    // T17-T17+N: golden cross-arch keygen vectors
    //
    // The vectors file (include/consensus/pq/test_vectors/
    // ml_dsa_65_keygen_vectors.h) is the authoritative cross-architecture
    // reproducibility anchor. Wallets on Apple Silicon, EPYC-x86-64, Windows,
    // Android, iOS MUST all produce pubkeys whose first 32 bytes match
    // these values for their respective seeds.
    //
    // Any mismatch is a wallet-portability emergency (see vector file
    // comment for failure modes).
    // -----------------------------------------------------------------------
    {
        namespace tv = dinero::consensus::pq::test_vectors;
        for (const auto& vec : tv::kMlDsa65KeygenVectors) {
            mldsa::Seed seed{};
            for (std::size_t i = 0; i < seed.size(); ++i) {
                seed[i] = vec.seed[i];
            }
            auto kp = mldsa::KeygenFromSeed(seed);

            std::printf("   [INFO] vector '%s' pubkey prefix: ", vec.name);
            for (std::size_t i = 0; i < 32; ++i) {
                std::printf("%02x", kp.pubkey[i]);
            }
            std::printf("\n");

            bool match = true;
            for (std::size_t i = 0; i < 32; ++i) {
                if (kp.pubkey[i] != vec.expected_pubkey_prefix[i]) {
                    match = false;
                    break;
                }
            }

            // Build a descriptive tag: "T17 vector: seed_all_zero"
            std::string tag = "T17 vector matches pin: ";
            tag += vec.name;
            record(match, tag.c_str());

            // Additional invariant: KeygenFromSeed is idempotent — call it
            // twice on the same seed and the full 1952-byte pubkey must be
            // bit-identical.
            auto kp_again = mldsa::KeygenFromSeed(seed);
            std::string tag_idem = "T17 vector idempotent: ";
            tag_idem += vec.name;
            record(kp_again.pubkey == kp.pubkey, tag_idem.c_str());
        }
    }

    // -----------------------------------------------------------------------
    // T18: cross-architecture verify canary.
    //
    // Pinned (seed, msg, signature) triple captured on Apple Silicon.
    // Every v7 host must Verify() the pinned signature successfully using
    // the pubkey derived from the same seed. Signing is hedged (non-
    // deterministic) so the triple is captured once and frozen; every
    // subsequent verification is pure-deterministic math.
    //
    // This test catches verifier drift that the keygen pins (T17) cannot:
    // two hosts could agree on a pubkey but disagree on verify in principle
    // (compiler bugs, platform-specific intrinsics, etc.). This pin closes
    // that gap.
    // -----------------------------------------------------------------------
    {
        namespace tv = dinero::consensus::pq::test_vectors;

        // Use the same seed as kMlDsa65KeygenVectors[0].
        mldsa::Seed canary_seed{};
        const auto& kv = tv::kMlDsa65KeygenVectors[0];
        for (std::size_t i = 0; i < canary_seed.size(); ++i) {
            canary_seed[i] = kv.seed[i];
        }
        auto canary_kp = mldsa::KeygenFromSeed(canary_seed);

        const auto msg_sv = tv::kSigCanaryMessage;
        const bool ok = mldsa::Verify(
            reinterpret_cast<const uint8_t*>(msg_sv.data()), msg_sv.size(),
            tv::kSigCanary_SeedA0AF.data(), tv::kSigCanary_SeedA0AF.size(),
            canary_kp.pubkey.data(), canary_kp.pubkey.size());
        record(ok, "T18: pinned signature verifies (cross-arch verify canary)");

        // Negative controls: tamper msg, sig, pubkey — each must fail.
        {
            std::string tamper_msg(msg_sv);
            tamper_msg[0] ^= 0x01;
            const bool bad = mldsa::Verify(
                reinterpret_cast<const uint8_t*>(tamper_msg.data()), tamper_msg.size(),
                tv::kSigCanary_SeedA0AF.data(), tv::kSigCanary_SeedA0AF.size(),
                canary_kp.pubkey.data(), canary_kp.pubkey.size());
            record(!bad, "T18a: tampered msg rejected against canary sig");
        }
        {
            std::array<uint8_t, dinero::consensus::pq::ml_dsa_65::SIGNATURE_BYTES> tamper_sig = tv::kSigCanary_SeedA0AF;
            tamper_sig[100] ^= 0x01;
            const bool bad = mldsa::Verify(
                reinterpret_cast<const uint8_t*>(msg_sv.data()), msg_sv.size(),
                tamper_sig.data(), tamper_sig.size(),
                canary_kp.pubkey.data(), canary_kp.pubkey.size());
            record(!bad, "T18b: tampered canary sig rejected");
        }
        {
            auto other_kp = mldsa::Keygen();
            const bool bad = mldsa::Verify(
                reinterpret_cast<const uint8_t*>(msg_sv.data()), msg_sv.size(),
                tv::kSigCanary_SeedA0AF.data(), tv::kSigCanary_SeedA0AF.size(),
                other_kp.pubkey.data(), other_kp.pubkey.size());
            record(!bad, "T18c: canary sig rejected under wrong pubkey");
        }
    }

    // -----------------------------------------------------------------------
    // T19: wallet-identity HKDF + KeygenFromSeed pipeline vector.
    //
    // The wallet schema (docs/consensus/V7_WALLET_SCHEMA.md) derives a
    // 32-byte PQ seed from a BIP-32 extended key using HKDF-SHA256. This
    // test pins:
    //   (a) the HKDF step: given canonical ikm/salt/info, the output
    //       must equal kHkdfVector_ExpectedPqSeed.
    //   (b) the end-to-end step: KeygenFromSeed over that pq_seed must
    //       produce the pinned pubkey prefix.
    //
    // Any wallet implementation passes this if its HKDF library agrees.
    // -----------------------------------------------------------------------
    {
        namespace tv = dinero::consensus::pq::test_vectors;

        // HKDF-SHA256 extract + expand using OpenSSL's HMAC API.
        auto hkdf_sha256 = [](const uint8_t* ikm, std::size_t ikm_len,
                              const uint8_t* salt, std::size_t salt_len,
                              const uint8_t* info, std::size_t info_len,
                              uint8_t* out, std::size_t L) -> bool {
            if (salt_len > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                L > 255U * 32U) {
                return false;
            }

            // Extract: PRK = HMAC(salt, ikm)
            unsigned int prk_len = 0;
            uint8_t prk[32];
            if (HMAC(EVP_sha256(), salt, static_cast<int>(salt_len),
                     ikm, ikm_len, prk, &prk_len) == nullptr ||
                prk_len != sizeof(prk)) {
                return false;
            }

            // Expand: T(i) = HMAC(PRK, T(i-1) || info || i)
            uint8_t prev[32];
            std::size_t prev_len = 0;
            std::size_t produced = 0;
            uint8_t counter = 1;
            while (produced < L) {
                std::vector<uint8_t> block_input;
                block_input.reserve(prev_len + info_len + 1);
                block_input.insert(block_input.end(), prev, prev + prev_len);
                block_input.insert(block_input.end(), info, info + info_len);
                block_input.push_back(counter);

                unsigned int tlen = 0;
                if (HMAC(EVP_sha256(), prk, static_cast<int>(prk_len),
                         block_input.data(), block_input.size(),
                         prev, &tlen) == nullptr ||
                    tlen != sizeof(prev)) {
                    return false;
                }

                prev_len = tlen;
                const std::size_t copy = (L - produced < tlen) ? (L - produced) : tlen;
                for (std::size_t i = 0; i < copy; ++i) out[produced + i] = prev[i];
                produced += copy;
                ++counter;
            }
            return true;
        };

        std::array<uint8_t, 32> derived_pq_seed{};
        const bool hkdf_ok = hkdf_sha256(
            tv::kHkdfVector_Ikm.data(),  tv::kHkdfVector_Ikm.size(),
            reinterpret_cast<const uint8_t*>(tv::kHkdfVector_Salt.data()),
            tv::kHkdfVector_Salt.size(),
            tv::kHkdfVector_Info.data(), tv::kHkdfVector_Info.size(),
            derived_pq_seed.data(),      derived_pq_seed.size());

        record(hkdf_ok && derived_pq_seed == tv::kHkdfVector_ExpectedPqSeed,
               "T19a: HKDF-SHA256(ikm, salt, info) matches pinned pq_seed");

        // Feed derived seed into KeygenFromSeed; check pubkey prefix.
        mldsa::Seed mld_seed{};
        for (std::size_t i = 0; i < mld_seed.size(); ++i) {
            mld_seed[i] = derived_pq_seed[i];
        }
        auto kp = mldsa::KeygenFromSeed(mld_seed);
        bool prefix_ok = true;
        for (std::size_t i = 0; i < 32; ++i) {
            if (kp.pubkey[i] != tv::kHkdfVector_ExpectedPubkeyPrefix[i]) {
                prefix_ok = false;
                break;
            }
        }
        record(prefix_ok,
               "T19b: KeygenFromSeed(pq_seed) matches pinned wallet-identity pubkey");
    }

    std::printf("----------------------------------------------------------------\n"
                " RESULT: %d passed, %d failed\n"
                "================================================================\n",
                g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
