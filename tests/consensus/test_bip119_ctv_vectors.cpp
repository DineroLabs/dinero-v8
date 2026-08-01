#include "consensus/covenants.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

using dinero::Transaction;
using dinero::TransactionSerializer;
using dinero::consensus::ComputeCTVHash;
using dinero::consensus::TryComputeCTVHash;
using dinero::consensus::VerifyCTV;

struct CTVVector {
    const char* name;
    const char* tx_hex;
    const char* expected_hash;
};

std::string ToHex(const std::array<uint8_t, 32>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const uint8_t byte : bytes) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

// Selected, compact CHECKTEMPLATEVERIFY congestion-control vectors from:
// https://github.com/bitcoin/bips/blob/master/bip-0119/vectors/tx_valid.json
//
// Keeping the transaction and expected digest together makes this an
// independent compatibility test: deriving the expected value with Dinero
// code would only prove that the implementation agrees with itself.
constexpr CTVVector kVectors[] = {
    {
        "level_0",
        "020000000146743e8df8b8b45936d30e67c775498ebd64cf6f172b9aca3b264391066d24d000000000000000000002781e0000000000002220bceb923d731eddf09705690d6ee697d1b3d57279ad96910cfb66a8d43a638800b3781e00000000000022201210e50077518896fd1661bffc2c1e88ad6e204ec4e8755407b42a97347c1e1bb300000000",
        "10618b50aa1120e9e940bbea8b6d081019e38fbb9b956b0ab4f61631d05727ca",
    },
    {
        "level_1",
        "0200000001f5dd5b069c8be8a5a527ff645b03116f921f649b3bbccfee5eb7eb476d869fb600000000000000000002480d00000000000022204c683607ca7950380df10647b223f656cd73d1f7ad67b30caf60d78337f913b2b3480d0000000000002220805bddd8b95a3cf3729b62703de37a1533b11634de76aa4a18605b640f1f3208b300000000",
        "bceb923d731eddf09705690d6ee697d1b3d57279ad96910cfb66a8d43a638800",
    },
    {
        "level_2",
        "02000000015447ea1eb8cb9124e7d8da1ef876ec2213f4f387968c4794fa040588331a337d00000000000000000002b004000000000000222084ac6e0fdbe91b09d1bf68158643ad68e0b3e64373738fb35711de0835011079b3b0040000000000002220241af26179a8e861cca473244723978127c16531f71fbcc33563c4f099651f8fb300000000",
        "4c683607ca7950380df10647b223f656cd73d1f7ad67b30caf60d78337f913b2",
    },
    {
        "level_3",
        "020000000170da0069a9de7de594bd749926e33c8208c46439ce15061a61d1027e35b230e60000000000000000000264000000000000001600141ca3bdf6bd2b1fc27420316a0d13f81fcdb85cb0640000000000000016001489d611c79700d6b4ae73d853ed49b86621d5802100000000",
        "84ac6e0fdbe91b09d1bf68158643ad68e0b3e64373738fb35711de0835011079",
    },
};

TEST(BIP119CTVVectors, MatchesUpstreamDefaultCheckTemplateVerifyHash) {
    for (const CTVVector& vector : kVectors) {
        SCOPED_TRACE(vector.name);

        const std::vector<uint8_t> raw =
            TransactionSerializer::FromHex(vector.tx_hex);
        ASSERT_FALSE(raw.empty());

        Transaction tx;
        size_t consumed = 0;
        ASSERT_TRUE(TransactionSerializer::Deserialize(tx, raw, consumed));
        ASSERT_EQ(consumed, raw.size());
        ASSERT_EQ(tx.vin.size(), 1U);
        ASSERT_FALSE(tx.vout.empty());
        for (const auto& output : tx.vout) {
            ASSERT_FALSE(output.is_confidential);
        }

        EXPECT_EQ(ToHex(ComputeCTVHash(tx, 0)), vector.expected_hash);
    }
}

TEST(BIP119CTVVectors, RejectsDineroSerializationExtensions) {
    Transaction tx;
    tx.vin.emplace_back();
    tx.vout.emplace_back();

    std::array<uint8_t, 32> hash{};
    tx.vout[0].is_confidential = true;
    EXPECT_FALSE(TryComputeCTVHash(tx, 0, hash));
    EXPECT_FALSE(VerifyCTV(tx, 0, std::vector<uint8_t>(32)));

    tx.vout[0].is_confidential = false;
    tx.has_explicit_fee = true;
    EXPECT_FALSE(TryComputeCTVHash(tx, 0, hash));

    tx.has_explicit_fee = false;
    tx.version = Transaction::TX_VERSION_SHIELDED;
    EXPECT_FALSE(TryComputeCTVHash(tx, 0, hash));
}

} // namespace
