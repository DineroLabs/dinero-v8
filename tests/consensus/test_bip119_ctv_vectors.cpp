#include "consensus/covenants.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "crypto/sha256.h"
#include "din_json.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
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

// ===========================================================================
// Complete upstream transaction/interpreter corpus (#483)
// ===========================================================================
//
// Provenance — both files are stored byte-for-byte as published upstream:
//
//   repo:   https://github.com/bitcoin/bips
//   commit: ae747e2b909ab5dd32632ed3a8b09839193d53e3
//   paths:  bip-0119/vectors/tx_valid.json
//           bip-0119/vectors/tx_invalid.json
//
// The runner deliberately maps only flags present in the pinned corpus. An
// unknown flag is a hard failure, never a warning or silent skip.

constexpr char kTxValidSha256[] =
    "e7b56a4b434b041c06f8d66a25ebd3a1acbc49efed96082d0f611a53115281e0";
constexpr char kTxInvalidSha256[] =
    "2dcae11cfcd6918eebff3f2a3233e8e55105e8d14bc1413b848c460f4eef7ea2";

constexpr int kExpectedValidCases = 19;
constexpr int kExpectedInvalidCases = 11;
constexpr int kExpectedValidInputs = 23;
constexpr int kExpectedInvalidInputs = 15;
constexpr int kExpectedConditionalCases = 9;
constexpr int kExpectedSkipExcludedCases = 9;

std::vector<uint8_t> DecodeHex(const std::string& hex) {
    if ((hex.size() & 1U) != 0) {
        throw std::runtime_error("odd-length hex token");
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        throw std::runtime_error("non-hex character");
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) |
                                           nibble(hex[i + 1])));
    }
    return out;
}

dinero::consensus::Script ParseVectorScript(const std::string& assembly) {
    using namespace dinero::consensus;
    const std::map<std::string, opcodetype> opcodes{
        {"OP_CHECKTEMPLATEVERIFY", OP_CHECKTEMPLATEVERIFY},
        {"OP_HASH160", OP_HASH160},
        {"OP_EQUAL", OP_EQUAL},
    };

    Script script;
    std::istringstream tokens(assembly);
    std::string token;
    while (tokens >> token) {
        if (token.rfind("0x", 0) == 0) {
            const auto bytes = DecodeHex(token.substr(2));
            script.data().insert(script.data().end(), bytes.begin(), bytes.end());
            continue;
        }
        const auto opcode = opcodes.find(token);
        if (opcode != opcodes.end()) {
            script << opcode->second;
            continue;
        }
        if (token == "0") {
            script << OP_0;
            continue;
        }
        if (token == "1") {
            script << OP_1;
            continue;
        }
        throw std::runtime_error("unsupported vector script token: " + token);
    }
    return script;
}

uint32_t ParseVectorFlags(const std::string& names) {
    using namespace dinero::consensus;
    const std::map<std::string, uint32_t> flags{
        {"NONE", SCRIPT_VERIFY_NONE},
        {"P2SH", SCRIPT_VERIFY_P2SH},
        {"WITNESS", SCRIPT_VERIFY_WITNESS},
        {"CLEANSTACK", SCRIPT_VERIFY_CLEANSTACK},
        {"DISCOURAGE_UPGRADABLE_NOPS",
         SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS},
        {"DEFAULT_CHECK_TEMPLATE_VERIFY_HASH",
         SCRIPT_VERIFY_CHECKTEMPLATEVERIFY},
    };

    uint32_t out = SCRIPT_VERIFY_NONE;
    std::istringstream list(names);
    std::string name;
    while (std::getline(list, name, ',')) {
        const auto it = flags.find(name);
        if (it == flags.end()) {
            throw std::runtime_error("unsupported BIP119 verify flag: " + name);
        }
        out |= it->second;
    }
    return out;
}

din::Json ReadPinnedJson(const char* path, const char* expected_sha256) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(std::string("vector corpus not readable: ") + path);
    }
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        throw std::runtime_error(std::string("empty vector corpus: ") + path);
    }
    if (Sha256Hex(bytes) != expected_sha256) {
        throw std::runtime_error(std::string("vector digest mismatch: ") + path);
    }
    din::Json root;
    if (!din::parse(std::string(bytes.begin(), bytes.end()), root) ||
        !root.isArray()) {
        throw std::runtime_error(std::string("invalid vector JSON: ") + path);
    }
    return root;
}

struct PrevoutData {
    dinero::uint256 txid;
    uint32_t vout{0};
    dinero::consensus::Script script;
    uint64_t amount{0};
};

std::vector<PrevoutData> ParsePrevouts(const din::Json& json) {
    if (!json.isArray()) {
        throw std::runtime_error("prevouts must be an array");
    }
    std::vector<PrevoutData> out;
    out.reserve(json.size());
    for (Json::ArrayIndex i = 0; i < json.size(); ++i) {
        const din::Json& item = json[i];
        if (!item.isArray() || item.size() != 4 ||
            !item[0].isString() || !item[1].isUInt() ||
            !item[2].isString() || !item[3].isUInt64()) {
            throw std::runtime_error("malformed prevout entry");
        }
        PrevoutData prevout;
        if (!dinero::uint256::FromHex(item[0].asString(), prevout.txid)) {
            throw std::runtime_error("invalid prevout txid");
        }
        prevout.vout = item[1].asUInt();
        prevout.script = ParseVectorScript(item[2].asString());
        prevout.amount = item[3].asUInt64();
        out.push_back(std::move(prevout));
    }
    return out;
}

struct VectorRunResult {
    bool all_inputs_valid{false};
    int inputs_checked{0};
    std::vector<int> script_errors;
};

VectorRunResult RunTransactionVector(const din::Json& entry, uint32_t flags) {
    using dinero::Transaction;
    using dinero::TransactionSerializer;
    using dinero::consensus::PrecomputedTransactionData;
    using dinero::consensus::Script;
    using dinero::consensus::ScriptError;
    using dinero::consensus::ScriptExecutionContext;
    using dinero::consensus::VerifyScript;

    const auto prevouts = ParsePrevouts(entry[0]);
    const auto raw = TransactionSerializer::FromHex(entry[1].asString());
    if (raw.empty()) {
        throw std::runtime_error("transaction hex decode failed");
    }
    Transaction tx;
    size_t consumed = 0;
    if (!TransactionSerializer::Deserialize(tx, raw, consumed) ||
        consumed != raw.size()) {
        throw std::runtime_error("transaction deserialization failed");
    }
    if (tx.vin.size() != prevouts.size()) {
        throw std::runtime_error("prevout count does not match transaction inputs");
    }

    std::vector<const PrevoutData*> input_prevouts;
    std::vector<uint64_t> all_amounts;
    std::vector<std::vector<uint8_t>> all_scripts;
    input_prevouts.reserve(tx.vin.size());
    all_amounts.reserve(tx.vin.size());
    all_scripts.reserve(tx.vin.size());
    for (const auto& input : tx.vin) {
        const auto match = std::find_if(
            prevouts.begin(), prevouts.end(), [&](const PrevoutData& prevout) {
                return input.prevout.txid.AsUint256() == prevout.txid &&
                       input.prevout.vout == prevout.vout;
            });
        if (match == prevouts.end()) {
            throw std::runtime_error("vector prevout does not match transaction input");
        }
        input_prevouts.push_back(&*match);
        all_amounts.push_back(match->amount);
        all_scripts.push_back(match->script.data());
    }

    const PrecomputedTransactionData precomputed(tx);
    VectorRunResult result{true, 0, {}};
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        ScriptExecutionContext context(
            &tx, static_cast<uint32_t>(i), input_prevouts[i]->amount, flags,
            all_amounts, all_scripts, {}, {}, &precomputed);
        ScriptError error = ScriptError::OK;
        const bool valid = VerifyScript(
            Script(tx.vin[i].scriptSig), input_prevouts[i]->script,
            tx.vin[i].witness, context, error);
        result.all_inputs_valid = result.all_inputs_valid && valid;
        result.script_errors.push_back(static_cast<int>(error));
        ++result.inputs_checked;
    }
    return result;
}

TEST(BIP119CTVVectors, MatchesUpstreamTransactionInterpreterCorpus) {
    using namespace dinero::consensus;
    const din::Json valid = ReadPinnedJson(
        DINERO_BIP119_TX_VALID_JSON, kTxValidSha256);
    const din::Json invalid = ReadPinnedJson(
        DINERO_BIP119_TX_INVALID_JSON, kTxInvalidSha256);

    int valid_cases = 0;
    int invalid_cases = 0;
    int valid_inputs = 0;
    int invalid_inputs = 0;
    int conditional_cases = 0;
    int skip_excluded_cases = 0;
    int staged_outer_witness_cases = 0;

    // Bitcoin's valid-vector schema lists flags to exclude from the standard
    // set, followed by flags that must always be included. Dinero's standard
    // set intentionally omits the policy-only NOP discouragement flag, so add
    // it to the starting set before applying the upstream exclusions.
    const uint32_t upstream_standard =
        SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS;
    for (Json::ArrayIndex i = 0; i < valid.size(); ++i) {
        const din::Json& entry = valid[i];
        if (!entry.isArray() || entry.size() <= 1) continue;
        ASSERT_GE(entry.size(), 3U) << "valid vector " << i;
        ASSERT_TRUE(entry[2].isString()) << "valid vector " << i;

        const uint32_t excluded = ParseVectorFlags(entry[2].asString());
        uint32_t included = SCRIPT_VERIFY_NONE;
        if (entry.size() >= 4) {
            ASSERT_TRUE(entry[3].isString()) << "valid vector " << i;
            included = ParseVectorFlags(entry[3].asString());
        }
        const uint32_t upstream_flags =
            (upstream_standard & ~excluded) | included;
        const uint32_t flags =
            upstream_flags | SCRIPT_VERIFY_CHECKTEMPLATEVERIFY;
        SCOPED_TRACE("valid BIP119 transaction vector " + std::to_string(i));
        const VectorRunResult result = RunTransactionVector(entry, flags);
        EXPECT_TRUE(result.all_inputs_valid)
            << "script errors=" << ::testing::PrintToString(result.script_errors);
        valid_inputs += result.inputs_checked;
        ++valid_cases;

        // These four baseline-valid vectors use P2WSH or Taproot script-path
        // execution without requesting CTV. Deployed Dinero nodes reject them
        // because the outer witness-program stack is not normalized. Pin that
        // legacy behavior, then prove the future covenant profile fixes it.
        // This makes the activation gate load-bearing and prevents an
        // accidental consensus-loosening change during a mixed-version rollout.
        if (i == 8 || i == 12 || i == 14 || i == 18) {
            EXPECT_FALSE(
                RunTransactionVector(entry, upstream_flags).all_inputs_valid);
            EXPECT_TRUE(result.all_inputs_valid);
            ++staged_outer_witness_cases;
        }

        if (entry.size() == 5) {
            ASSERT_TRUE(entry[4].isBool());
            EXPECT_TRUE(entry[4].asBool());
            ++skip_excluded_cases;
        }
    }

    // Invalid vectors provide the exact verification flags. Conditional
    // clauses describe the soft-fork-compatible alternate case: when CTV is
    // unset, DISCOURAGE_UPGRADABLE_NOPS must also be unset and the transaction
    // becomes valid under historical NOP4 behavior. Exercise both outcomes.
    for (Json::ArrayIndex i = 0; i < invalid.size(); ++i) {
        const din::Json& entry = invalid[i];
        if (!entry.isArray() || entry.size() <= 1) continue;
        ASSERT_GE(entry.size(), 3U) << "invalid vector " << i;
        ASSERT_TRUE(entry[2].isString()) << "invalid vector " << i;

        const uint32_t flags =
            ParseVectorFlags(entry[2].asString()) |
            SCRIPT_VERIFY_CHECKTEMPLATEVERIFY;
        SCOPED_TRACE("invalid BIP119 transaction vector " + std::to_string(i));
        const VectorRunResult result = RunTransactionVector(entry, flags);
        EXPECT_FALSE(result.all_inputs_valid);
        invalid_inputs += result.inputs_checked;
        ++invalid_cases;

        if (entry.size() == 4) {
            ASSERT_TRUE(entry[3].isArray());
            for (Json::ArrayIndex c = 0; c < entry[3].size(); ++c) {
                const din::Json& condition = entry[3][c];
                ASSERT_TRUE(condition.isObject());
                ASSERT_TRUE(condition["if_unset"].isArray());
                ASSERT_TRUE(condition["then_unset"].isArray());
                uint32_t alternate_flags = flags;
                for (const char* key : {"if_unset", "then_unset"}) {
                    const din::Json& names = condition[key];
                    for (Json::ArrayIndex n = 0; n < names.size(); ++n) {
                        ASSERT_TRUE(names[n].isString());
                        alternate_flags &= ~ParseVectorFlags(names[n].asString());
                    }
                }
                EXPECT_TRUE(RunTransactionVector(entry, alternate_flags)
                                .all_inputs_valid)
                    << "historical NOP4 alternate must remain valid";
                ++conditional_cases;
            }
        }
    }

    EXPECT_EQ(valid_cases, kExpectedValidCases);
    EXPECT_EQ(invalid_cases, kExpectedInvalidCases);
    EXPECT_EQ(valid_inputs, kExpectedValidInputs);
    EXPECT_EQ(invalid_inputs, kExpectedInvalidInputs);
    EXPECT_EQ(conditional_cases, kExpectedConditionalCases);
    EXPECT_EQ(skip_excluded_cases, kExpectedSkipExcludedCases);
    EXPECT_EQ(staged_outer_witness_cases, 4);
}

} // namespace
