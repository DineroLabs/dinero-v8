/**
 * @file test_wallet_psbt_policy_context.cpp
 * @brief Verifies walletprocesspsbt uses active wallet policy from active DB context.
 */

#include "daemon/execution_context.h"
#include "wallet/wallet_manager.h"
#include "wallet/wallet_iface.h"
#include "wallet/psbt_witness_utxo_decode.h"
#include "dinero/core/wallet/psbt.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace din {
namespace rpc {
Json::Value walletprocesspsbt_handler(const ExecutionContext& ctx, const Json::Value& params);
}  // namespace rpc
}  // namespace din

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define ASSERT_TRUE(cond, msg)                                                    \
    do {                                                                          \
        g_tests_run++;                                                            \
        if (!(cond)) {                                                            \
            std::cerr << "  FAIL: " << msg << "\n";                               \
            std::cerr << "    at " << __FILE__ << ":" << __LINE__ << "\n";      \
            return false;                                                         \
        }                                                                         \
        g_tests_passed++;                                                         \
    } while (0)

enum class WitnessUtxoEncodingMode {
    Canonical,
    LegacyMissingCompactSize,
    CanonicalWithTrailingGarbage,
    CanonicalWithTruncatedScript,
};

static void PutCompactSize(std::vector<uint8_t>& out, uint64_t v) {
    if (v < 0xFD) {
        out.push_back(static_cast<uint8_t>(v));
        return;
    }
    if (v <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        return;
    }
    if (v <= 0xFFFFFFFFULL) {
        out.push_back(0xFE);
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        return;
    }
    out.push_back(0xFF);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

static std::vector<uint8_t> BuildCanonicalWitnessUtxoValue(const std::vector<uint8_t>& script_pubkey,
                                                            uint64_t amount_sats) {
    std::vector<uint8_t> out;
    out.reserve(8 + 9 + script_pubkey.size());
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((amount_sats >> (8 * i)) & 0xFF));
    }
    PutCompactSize(out, script_pubkey.size());
    out.insert(out.end(), script_pubkey.begin(), script_pubkey.end());
    return out;
}

class AlwaysSigningKeyStore : public din::IKeyStore {
public:
    std::optional<std::string> getXPub(const std::string&) const override {
        return std::nullopt;
    }

    std::optional<std::string> getXPriv(const std::string&) const override {
        return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> sign(const std::vector<uint8_t>&, const std::string&) override {
        // Schnorr signature length expected by taproot path in signer.
        return std::vector<uint8_t>(64, 0x42);
    }

    bool canSign(const std::string&) const override {
        return true;
    }

    bool hasKey(const std::string&) const override {
        return true;
    }

    std::vector<std::string> listKeyPaths() const override {
        return {"m"};
    }
};

static fs::path MakeTempDir() {
    const auto nonce = static_cast<unsigned long long>(std::rand());
    fs::path dir = fs::temp_directory_path() /
                   ("dinero_wallet_psbt_policy_ctx_" + std::to_string(nonce));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static bool SetWalletPolicy(dinero::WalletManager& wallet_manager,
                            const std::string& wallet_name,
                            const std::string& wallet_policy) {
    wallet_manager.open(wallet_name);
    sqlite3* db = wallet_manager.getCurrentDatabase();
    if (!db) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE wallet_meta SET wallet_policy = ? WHERE id = 1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, wallet_policy.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static void AddLegacyWitnessUtxoEncoding(din::Psbt& psbt,
                                         size_t input_index,
                                         const std::vector<uint8_t>& script_pubkey,
                                         uint64_t amount_sats) {
    // Legacy malformed encoding used by older wallet code:
    // <8-byte amount><raw script_pubkey> (missing CompactSize script length).
    std::vector<uint8_t> value;
    value.reserve(8 + script_pubkey.size());
    for (int i = 0; i < 8; ++i) {
        value.push_back(static_cast<uint8_t>((amount_sats >> (8 * i)) & 0xFF));
    }
    value.insert(value.end(), script_pubkey.begin(), script_pubkey.end());

    std::vector<uint8_t> key = {static_cast<uint8_t>(din::PsbtIn::WitnessUtxo)};
    psbt.inputs[input_index].kv.emplace_back(std::move(key), std::move(value));
}

static bool AppendWitnessUtxoTrailingGarbage(din::Psbt& psbt, size_t input_index) {
    if (input_index >= psbt.inputs.size()) {
        return false;
    }
    for (auto& kv : psbt.inputs[input_index].kv) {
        if (!kv.key.empty() &&
            kv.key[0] == static_cast<uint8_t>(din::PsbtIn::WitnessUtxo)) {
            kv.value.push_back(0xEE);
            return true;
        }
    }
    return false;
}

static bool TruncateWitnessUtxoScriptByte(din::Psbt& psbt, size_t input_index) {
    if (input_index >= psbt.inputs.size()) {
        return false;
    }
    for (auto& kv : psbt.inputs[input_index].kv) {
        if (!kv.key.empty() &&
            kv.key[0] == static_cast<uint8_t>(din::PsbtIn::WitnessUtxo)) {
            if (kv.value.size() <= 9) {
                return false;
            }
            kv.value.pop_back();
            return true;
        }
    }
    return false;
}

static std::string BuildTaprootScriptPathPsbtBase64(WitnessUtxoEncodingMode witness_encoding_mode) {
    din::Psbt psbt;
    din::add_global_version(psbt, 2);
    psbt.inputs.resize(1);

    din::add_global_input_count(psbt, 1);
    din::add_global_output_count(psbt, 0);

    std::vector<uint8_t> prev_txid(32, 0x11);
    din::add_in_prev_txid(psbt, 0, prev_txid);
    din::add_in_output_index(psbt, 0, 0);

    // Taproot witness UTXO scriptPubKey: OP_1 PUSH32 <xonly>
    std::vector<uint8_t> taproot_spk;
    taproot_spk.reserve(34);
    taproot_spk.push_back(0x51);
    taproot_spk.push_back(0x20);
    taproot_spk.insert(taproot_spk.end(), 32, 0x22);
    if (witness_encoding_mode == WitnessUtxoEncodingMode::LegacyMissingCompactSize) {
        AddLegacyWitnessUtxoEncoding(psbt, 0, taproot_spk, 1000);
    } else {
        din::add_in_witness_utxo(psbt, 0, taproot_spk, 1000);
        if (witness_encoding_mode == WitnessUtxoEncodingMode::CanonicalWithTrailingGarbage) {
            const bool mutated = AppendWitnessUtxoTrailingGarbage(psbt, 0);
            if (!mutated) {
                return {};
            }
        } else if (witness_encoding_mode == WitnessUtxoEncodingMode::CanonicalWithTruncatedScript) {
            const bool mutated = TruncateWitnessUtxoScriptByte(psbt, 0);
            if (!mutated) {
                return {};
            }
        }
    }

    // Script-path indicator for guardrail trigger (BIP371 TAP_LEAF_SCRIPT = 0x15).
    std::vector<uint8_t> tap_leaf_key;
    tap_leaf_key.reserve(34);
    tap_leaf_key.push_back(0x15);
    tap_leaf_key.insert(tap_leaf_key.end(), 33, 0x01);  // dummy control block

    // Value = <script><leaf_version>; use simple OP_TRUE script and tapscript leaf version.
    std::vector<uint8_t> tap_leaf_value = {0x51, 0xC0};
    psbt.inputs[0].kv.emplace_back(tap_leaf_key, tap_leaf_value);

    const auto raw = din::serialize(psbt);
    return din::to_base64(raw);
}

static bool AssertPolicyBehaviorForPsbt(dinero::WalletManager& wallet_manager,
                                        din::ExecutionContext& ctx,
                                        const std::string& psbt_b64,
                                        const std::string& fixture_name) {
    Json::Value params(Json::objectValue);
    params["psbt"] = psbt_b64;
    params["sign"] = true;

    wallet_manager.open("policy_bip86");
    Json::Value bip86_resp = din::rpc::walletprocesspsbt_handler(ctx, params);
    ASSERT_TRUE(bip86_resp.isMember("error"),
                fixture_name + ": bip86 response must return an error for script-path taproot");
    ASSERT_TRUE(bip86_resp["error"].isMember("message"),
                fixture_name + ": bip86 error must include message");
    const std::string bip86_error = bip86_resp["error"]["message"].asString();
    ASSERT_TRUE(bip86_error.find("Policy violation") != std::string::npos,
                fixture_name + ": bip86 error must be explicit policy violation");

    wallet_manager.open("policy_bip84");
    Json::Value bip84_resp = din::rpc::walletprocesspsbt_handler(ctx, params);
    ASSERT_TRUE(bip84_resp.isMember("error"),
                fixture_name + ": bip84 response should still fail with synthetic PSBT");
    ASSERT_TRUE(bip84_resp["error"].isMember("message"),
                fixture_name + ": bip84 error must include message");
    const std::string bip84_error = bip84_resp["error"]["message"].asString();
    ASSERT_TRUE(bip84_error.find("Policy violation") == std::string::npos,
                fixture_name + ": bip84 must not reject on BIP86 policy guardrail");
    ASSERT_TRUE(bip84_error.find("incomplete") != std::string::npos ||
                    bip84_error.find("no inputs could be signed") != std::string::npos,
                fixture_name + ": bip84 failure should be signing completeness, not policy guardrail");

    return true;
}

static bool AssertNoPolicyViolationForPsbt(dinero::WalletManager& wallet_manager,
                                           din::ExecutionContext& ctx,
                                           const std::string& psbt_b64,
                                           const std::string& fixture_name) {
    Json::Value params(Json::objectValue);
    params["psbt"] = psbt_b64;
    params["sign"] = true;

    wallet_manager.open("policy_bip86");
    Json::Value bip86_resp = din::rpc::walletprocesspsbt_handler(ctx, params);
    ASSERT_TRUE(bip86_resp.isMember("error"),
                fixture_name + ": bip86 response must fail for malformed synthetic PSBT");
    ASSERT_TRUE(bip86_resp["error"].isMember("message"),
                fixture_name + ": bip86 error must include message");
    const std::string bip86_error = bip86_resp["error"]["message"].asString();
    ASSERT_TRUE(bip86_error.find("Policy violation") == std::string::npos,
                fixture_name + ": bip86 must not trigger policy guardrail on malformed witness_utxo");

    wallet_manager.open("policy_bip84");
    Json::Value bip84_resp = din::rpc::walletprocesspsbt_handler(ctx, params);
    ASSERT_TRUE(bip84_resp.isMember("error"),
                fixture_name + ": bip84 response should fail with synthetic PSBT");
    ASSERT_TRUE(bip84_resp["error"].isMember("message"),
                fixture_name + ": bip84 error must include message");
    const std::string bip84_error = bip84_resp["error"]["message"].asString();
    ASSERT_TRUE(bip84_error.find("Policy violation") == std::string::npos,
                fixture_name + ": bip84 must not reject on BIP86 policy guardrail");

    return true;
}

static bool TestWitnessUtxoDecodeRules() {
    std::cout << "\n[TEST] witness_utxo decode strictness and fallback rules" << std::endl;

    std::vector<uint8_t> taproot_spk;
    taproot_spk.reserve(34);
    taproot_spk.push_back(0x51);
    taproot_spk.push_back(0x20);
    taproot_spk.insert(taproot_spk.end(), 32, 0x22);

    const auto canonical_value = BuildCanonicalWitnessUtxoValue(taproot_spk, 1000);
    const auto canonical = din::psbt::DecodeWitnessUtxoValue(canonical_value);
    ASSERT_TRUE(canonical.ok, "canonical witness_utxo must decode");
    ASSERT_TRUE(!canonical.used_legacy_fallback, "canonical witness_utxo must not use legacy fallback");
    ASSERT_TRUE(canonical.amount == 1000, "canonical witness_utxo amount must match");
    ASSERT_TRUE(canonical.script_pubkey == taproot_spk, "canonical witness_utxo script must match");

    std::vector<uint8_t> legacy_decode_failure_value;
    legacy_decode_failure_value.reserve(9);
    for (int i = 0; i < 8; ++i) {
        legacy_decode_failure_value.push_back(0x00);
    }
    // 0xFD starts a 3-byte CompactSize varint but there are no following bytes.
    legacy_decode_failure_value.push_back(0xFD);
    const auto legacy_decode_failure = din::psbt::DecodeWitnessUtxoValue(legacy_decode_failure_value);
    ASSERT_TRUE(legacy_decode_failure.ok, "legacy decode-failure payload should trigger fallback");
    ASSERT_TRUE(legacy_decode_failure.used_legacy_fallback,
                "legacy decode-failure payload must mark fallback usage");
    ASSERT_TRUE(legacy_decode_failure.script_pubkey.size() == 1 &&
                    legacy_decode_failure.script_pubkey[0] == 0xFD,
                "legacy decode-failure payload must preserve raw script bytes");

    auto canonical_bad_consumption = canonical_value;
    canonical_bad_consumption.pop_back();
    const auto bad_consumption = din::psbt::DecodeWitnessUtxoValue(canonical_bad_consumption);
    ASSERT_TRUE(!bad_consumption.ok,
                "canonical witness_utxo with non-exact consumption must be rejected");

    auto canonical_trailing = canonical_value;
    canonical_trailing.push_back(0xEE);
    const auto trailing = din::psbt::DecodeWitnessUtxoValue(canonical_trailing);
    ASSERT_TRUE(!trailing.ok,
                "canonical witness_utxo with trailing bytes must be rejected");

    std::cout << "  PASS: witness_utxo strict decode and fallback rules enforced" << std::endl;
    return true;
}

static bool TestWalletProcessPsbtPolicyFollowsActiveWallet() {
    std::cout << "\n[TEST] walletprocesspsbt active-wallet policy context" << std::endl;

    const fs::path datadir = MakeTempDir();
    dinero::WalletManager wallet_manager(datadir);
    wallet_manager.create("policy_bip86");
    wallet_manager.create("policy_bip84");

    ASSERT_TRUE(SetWalletPolicy(wallet_manager, "policy_bip86", "bip86"),
                "failed to set policy_bip86 wallet policy to bip86");
    ASSERT_TRUE(SetWalletPolicy(wallet_manager, "policy_bip84", "bip84"),
                "failed to set policy_bip84 wallet policy to bip84");

    AlwaysSigningKeyStore keystore;
    din::ExecutionContext ctx;
    ctx.wallet_manager = &wallet_manager;
    ctx.key_store = &keystore;
    ASSERT_TRUE(AssertPolicyBehaviorForPsbt(
                    wallet_manager,
                    ctx,
                    BuildTaprootScriptPathPsbtBase64(WitnessUtxoEncodingMode::Canonical),
                    "canonical witness_utxo"),
                "canonical witness_utxo policy assertions failed");
    ASSERT_TRUE(AssertNoPolicyViolationForPsbt(
                    wallet_manager,
                    ctx,
                    BuildTaprootScriptPathPsbtBase64(WitnessUtxoEncodingMode::LegacyMissingCompactSize),
                    "legacy missing-compactsize witness_utxo"),
                "legacy missing-compactsize witness_utxo assertions failed");
    ASSERT_TRUE(AssertNoPolicyViolationForPsbt(
                    wallet_manager,
                    ctx,
                    BuildTaprootScriptPathPsbtBase64(WitnessUtxoEncodingMode::CanonicalWithTruncatedScript),
                    "canonical witness_utxo with truncated script"),
                "canonical witness_utxo truncated script assertions failed");
    ASSERT_TRUE(AssertNoPolicyViolationForPsbt(
                    wallet_manager,
                    ctx,
                    BuildTaprootScriptPathPsbtBase64(WitnessUtxoEncodingMode::CanonicalWithTrailingGarbage),
                    "canonical witness_utxo with trailing garbage"),
                "canonical witness_utxo trailing garbage assertions failed");

    std::error_code ec;
    fs::remove_all(datadir, ec);

    std::cout << "  PASS: walletprocesspsbt policy follows active wallet context"
              << " (strict canonical witness_utxo consumption)" << std::endl;
    return true;
}

int main() {
    std::srand(0xA11CE);

    if (!TestWitnessUtxoDecodeRules() ||
        !TestWalletProcessPsbtPolicyFollowsActiveWallet()) {
        std::cerr << "\nFAILED: " << g_tests_passed << "/" << g_tests_run << " assertions passed\n";
        return 1;
    }

    std::cout << "\nPASS: " << g_tests_passed << "/" << g_tests_run << " assertions passed\n";
    return 0;
}
