#include "consensus/covenants.h"
#include "crypto/sha256.h"
#include "din_json.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <fstream>
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

// ===========================================================================
// Complete upstream CTV hash corpus
// ===========================================================================
//
// The four vectors above are the human-readable congestion-control examples.
// This section runs the ENTIRE upstream corpus, which is fuzz-generated and
// covers input/output counts, witness presence, non-empty scriptSigs, and
// arbitrary nVersion values that hand-picked examples never reach.
//
// Provenance — the file is stored byte-for-byte as published upstream and
// pinned by digest, so it cannot drift or be edited to fit our implementation:
//
//   repo:   https://github.com/bitcoin/bips
//   commit: ae747e2b909ab5dd32632ed3a8b09839193d53e3
//   path:   bip-0119/vectors/ctvhash.json
//   sha256: 3cff1abe3284b9d05fa95422724680f0ffdaa69675fc3c679de688c872174160
//
// Upstream schema (first array element documents it):
//   {"hex_tx": <hex>, "spend_index": [n...], "result": [<hex hash>...]}

namespace {

constexpr char kVectorFileSha256[] =
    "3cff1abe3284b9d05fa95422724680f0ffdaa69675fc3c679de688c872174160";

// Exact coverage pins. These are constants because the vector file is pinned
// by digest, so the counts are deterministic. Pinning the totals — rather than
// asserting "no mismatches" alone — means the test cannot silently degrade to
// covering nothing if parsing or iteration regresses.
constexpr int kExpectedVectors = 100;
constexpr int kExpectedPairs = 400;
constexpr int kExpectedComputable = 193;
constexpr int kExpectedOutOfRange = 207;

std::string Sha256Hex(const std::vector<uint8_t>& bytes) {
    dinero::crypto::CSHA256 sha;
    sha.Write(bytes.data(), bytes.size());
    uint8_t digest[32];
    sha.Finalize(digest);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const uint8_t byte : digest) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

} // namespace

TEST(BIP119CTVVectors, MatchesCompleteUpstreamCtvhashCorpus) {
    // A missing or unreadable corpus is a hard failure, never a silent skip:
    // a vector test that quietly covers zero vectors is worse than no test.
    std::ifstream file(DINERO_BIP119_CTVHASH_JSON, std::ios::binary);
    ASSERT_TRUE(file.is_open())
        << "upstream CTV vector corpus not readable at "
        << DINERO_BIP119_CTVHASH_JSON;

    const std::vector<uint8_t> raw_file(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    ASSERT_FALSE(raw_file.empty());

    // Provenance gate. If this fires, the vector file was modified locally.
    // Re-download from the pinned upstream commit; do NOT relax this to match
    // an edited file, and do not change Dinero's implementation to satisfy a
    // corpus that no longer matches upstream.
    ASSERT_EQ(Sha256Hex(raw_file), kVectorFileSha256)
        << "tests/vectors/bip119_ctvhash.json does not match the pinned "
           "upstream digest";

    din::Json root;
    ASSERT_TRUE(din::parse(
        std::string(raw_file.begin(), raw_file.end()), root));
    ASSERT_TRUE(root.isArray());

    int vectors = 0;
    int pairs = 0;
    int computable = 0;
    int out_of_range = 0;

    for (Json::ArrayIndex v = 0; v < root.size(); ++v) {
        const din::Json& entry = root[v];
        // The upstream file's first element is a schema description string.
        if (!entry.isObject()) {
            continue;
        }
        ++vectors;

        const std::string hex_tx = entry["hex_tx"].asString();
        const din::Json& indices = entry["spend_index"];
        const din::Json& results = entry["result"];
        ASSERT_TRUE(indices.isArray());
        ASSERT_TRUE(results.isArray());
        ASSERT_EQ(indices.size(), results.size());

        const std::string desc =
            entry.isMember("desc") ? entry["desc"].toStyledString()
                                   : std::string("(no desc)");

        const std::vector<uint8_t> raw = TransactionSerializer::FromHex(hex_tx);
        ASSERT_FALSE(raw.empty()) << "vector " << v << " hex decode failed";

        Transaction tx;
        size_t consumed = 0;
        // Every upstream transaction must round-trip through Dinero's
        // deserializer. A parse failure here would mean Dinero cannot even
        // represent a transaction whose CTV hash the BIP defines.
        ASSERT_TRUE(TransactionSerializer::Deserialize(tx, raw, consumed))
            << "vector " << v << " (" << desc << ") failed to deserialize";
        ASSERT_EQ(consumed, raw.size()) << "vector " << v << " trailing bytes";

        for (Json::ArrayIndex i = 0; i < indices.size(); ++i) {
            ++pairs;
            const uint64_t wide_index = indices[i].asUInt64();
            const std::string expected = results[i].asString();
            const uint32_t index = static_cast<uint32_t>(wide_index);

            SCOPED_TRACE(
                "vector " + std::to_string(v) + " spend_index " +
                std::to_string(wide_index) + " desc " + desc);

            std::array<uint8_t, 32> got{};
            const bool computed = TryComputeCTVHash(tx, index, got);

            if (index < tx.vin.size()) {
                // In-range: Dinero must compute, and must agree with upstream.
                ++computable;
                ASSERT_TRUE(computed)
                    << "refused an in-range input index (vin.size="
                    << tx.vin.size() << ")";
                EXPECT_EQ(ToHex(got), expected);
            } else {
                // Out-of-range: upstream still defines a digest here because
                // the index is merely serialized into the preimage, but CTV is
                // only ever evaluated at a real input during script execution.
                // Dinero deliberately fails closed instead. Assert the refusal
                // rather than skipping, so the divergence stays intentional
                // and cannot silently become "computes a wrong hash".
                ++out_of_range;
                EXPECT_FALSE(computed)
                    << "computed a digest for an out-of-range input index "
                       "(vin.size=" << tx.vin.size() << ")";
            }
        }
    }

    EXPECT_EQ(vectors, kExpectedVectors);
    EXPECT_EQ(pairs, kExpectedPairs);
    EXPECT_EQ(computable, kExpectedComputable);
    EXPECT_EQ(out_of_range, kExpectedOutOfRange);
}

} // namespace
