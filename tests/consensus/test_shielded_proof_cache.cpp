#include <gtest/gtest.h>
#include "consensus/shielded/proof_verification_cache.h"

#include <thread>
#include <vector>

namespace dinero::consensus::shielded::detail {
namespace {

TEST(ShieldedProofCache, KeysSeparateEveryPublicInputProfileAndProofByte) {
    const std::vector<uint8_t> proof{6, 1, 2, 3};
    SpendPublicInputs spend{};
    OutputPublicInputs output{};
    const auto spend_key = SpendProofCacheKey(proof, spend, true, true, true, true);
    const auto output_key = OutputProofCacheKey(proof, output, true, true);
    EXPECT_NE(spend_key, output_key);
    EXPECT_NE(spend_key, SpendProofCacheKey(proof, spend, false, true, true, true));
    EXPECT_NE(spend_key, SpendProofCacheKey(proof, spend, true, false, true, true));
    EXPECT_NE(spend_key, SpendProofCacheKey(proof, spend, true, true, false, true));
    EXPECT_NE(spend_key, SpendProofCacheKey(proof, spend, true, true, true, false));
    EXPECT_NE(output_key, OutputProofCacheKey(proof, output, false, true));
    EXPECT_NE(output_key, OutputProofCacheKey(proof, output, true, false));
    for (size_t i = 0; i < proof.size(); ++i) {
        auto changed = proof; changed[i] ^= 1;
        EXPECT_NE(spend_key, SpendProofCacheKey(changed, spend, true, true, true, true));
        EXPECT_NE(output_key, OutputProofCacheKey(changed, output, true, true));
    }
    auto extended = proof; extended.push_back(0);
    EXPECT_NE(spend_key, SpendProofCacheKey(extended, spend, true, true, true, true));
    EXPECT_NE(output_key, OutputProofCacheKey(extended, output, true, true));
    for (size_t i = 0; i < 32; ++i) {
        auto s = spend; s.nullifier[i] = 1;
        EXPECT_NE(spend_key, SpendProofCacheKey(proof, s, true, true, true, true));
        s = spend; s.anchor[i] = 1;
        EXPECT_NE(spend_key, SpendProofCacheKey(proof, s, true, true, true, true));
        s = spend; s.covenant_outputs[i] = 1;
        EXPECT_NE(spend_key, SpendProofCacheKey(proof, s, true, true, true, true));
        auto o = output; o.commitment[i] = 1;
        EXPECT_NE(output_key, OutputProofCacheKey(proof, o, true, true));
    }
    for (size_t i = 0; i < 33; ++i) {
        auto s = spend; s.cv[i] = 1;
        EXPECT_NE(spend_key, SpendProofCacheKey(proof, s, true, true, true, true));
        auto o = output; o.cv[i] = 1;
        EXPECT_NE(output_key, OutputProofCacheKey(proof, o, true, true));
    }
    for (size_t i = 0; i < 4; ++i) {
        auto s = spend; s.covenant_minimum_height = uint32_t{1} << (8 * i);
        EXPECT_NE(spend_key, SpendProofCacheKey(proof, s, true, true, true, true));
    }
}

TEST(ShieldedProofCache, BoundedDuplicateSafeEvictionAndConcurrentReads) {
    VerifiedProofCache<2> cache;
    Hash a{}, b{}, c{}; a[0] = 1; b[0] = 2; c[0] = 3;
    EXPECT_FALSE(cache.Contains(a));
    cache.RememberVerified(a);
    cache.RememberVerified(b);
    cache.RememberVerified(a);  // A duplicate must not consume a slot.
    EXPECT_TRUE(cache.Contains(a)); EXPECT_TRUE(cache.Contains(b));
    cache.RememberVerified(c);
    EXPECT_FALSE(cache.Contains(a)); EXPECT_TRUE(cache.Contains(b)); EXPECT_TRUE(cache.Contains(c));
    std::vector<std::thread> readers;
    for (size_t i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            for (size_t j = 0; j < 1000; ++j) {
                cache.RememberVerified(b);
                EXPECT_TRUE(cache.Contains(b)); EXPECT_TRUE(cache.Contains(c));
            }
        });
    }
    for (auto& reader : readers) reader.join();
}

} // namespace
} // namespace dinero::consensus::shielded::detail
