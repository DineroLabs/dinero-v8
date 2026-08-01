#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "consensus/covenants.h"
#include "consensus/interfaces/iutxo_provider.h"
#include "consensus/script_interpreter.h"
#include "consensus/transaction_validator.h"
#include "consensus/tx_validation.h"
#include "consensus/utxo_entry.h"
#include "primitives/hash_domains.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "policy/covenant_relay_policy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace {

using dinero::Chain;
using dinero::ChainParams;
using dinero::ConsensusChecksum;
using dinero::OutPoint;
using dinero::Params;
using dinero::SelectParams;
using dinero::Transaction;
using dinero::TransactionValidator;
using dinero::TxId;
using dinero::TxInput;
using dinero::TxOutput;
using dinero::consensus::CovenantActivationParams;
using dinero::consensus::PrecomputedTransactionData;
using dinero::consensus::ContractState;
using dinero::consensus::IUTXOProvider;
using dinero::consensus::UTXOEntry;

class OneCoinProvider final : public IUTXOProvider {
public:
    OneCoinProvider(OutPoint outpoint, UTXOEntry coin)
        : outpoint_(std::move(outpoint)), coin_(std::move(coin)) {}

    std::optional<UTXOEntry> GetUTXO(
        const OutPoint& outpoint) const override {
        if (outpoint == outpoint_) return coin_;
        return std::nullopt;
    }

    bool AddUTXO(const OutPoint&, const UTXOEntry&) override {
        return false;
    }
    bool SpendUTXO(const OutPoint&, uint32_t) override {
        return false;
    }
    bool DeleteUTXO(const OutPoint&) override {
        return false;
    }
    bool HasUTXO(const OutPoint& outpoint) const override {
        return outpoint == outpoint_;
    }

private:
    OutPoint outpoint_;
    UTXOEntry coin_;
};

TEST(CovenantActivation, ProductionNetworksPinReviewedActivationPolicy) {
    SelectParams(Chain::MAINNET);
    EXPECT_EQ(Params().taproot_scriptpath_activation_height, 1U);
    EXPECT_EQ(Params().ctv_activation_height, 80000U);
    EXPECT_EQ(Params().csfs_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().txhash_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().ccv_activation_height, 80000U);
    EXPECT_EQ(CovenantActivationParams::CovenantFlags(79999, Params()),
              dinero::consensus::SCRIPT_VERIFY_NONE);
    EXPECT_EQ(CovenantActivationParams::CovenantFlags(80000, Params()),
              dinero::consensus::SCRIPT_VERIFY_CHECKTEMPLATEVERIFY |
                  dinero::consensus::SCRIPT_VERIFY_CHECKCONTRACT);

    SelectParams(Chain::TESTNET);
    EXPECT_EQ(Params().taproot_scriptpath_activation_height, 200U);
    EXPECT_EQ(Params().ctv_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().ccv_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().csfs_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().txhash_activation_height, UINT32_MAX);
    EXPECT_EQ(CovenantActivationParams::CovenantFlags(
                  UINT32_MAX, Params()),
              dinero::consensus::SCRIPT_VERIFY_NONE);
}

TEST(CovenantActivation, RegtestExercisesReviewedCovenants) {
    SelectParams(Chain::REGTEST);
    EXPECT_FALSE(CovenantActivationParams::IsScriptPathActive(19, Params()));
    EXPECT_TRUE(CovenantActivationParams::IsScriptPathActive(20, Params()));
    EXPECT_EQ(CovenantActivationParams::CovenantFlags(19, Params()),
              dinero::consensus::SCRIPT_VERIFY_NONE);
    EXPECT_EQ(CovenantActivationParams::CovenantFlags(20, Params()),
              dinero::consensus::SCRIPT_VERIFY_CHECKTEMPLATEVERIFY |
                  dinero::consensus::SCRIPT_VERIFY_CHECKCONTRACT);
    EXPECT_EQ(Params().csfs_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().txhash_activation_height, UINT32_MAX);
}

TEST(CovenantActivation, RelayPolicyRejectsDormantRevealedOpcodes) {
    ChainParams params{};
    params.taproot_scriptpath_activation_height = 1;
    params.ctv_activation_height = 20;
    params.ccv_activation_height = 30;
    params.csfs_activation_height = UINT32_MAX;
    params.txhash_activation_height = UINT32_MAX;

    auto transactionWithScript = [](std::vector<uint8_t> tapscript) {
        Transaction tx;
        tx.vin.emplace_back();
        tx.vout.emplace_back(
            dinero::AmountUna::Una(1),
            std::vector<uint8_t>{dinero::consensus::OP_TRUE});
        tx.vin[0].witness = {
            std::move(tapscript),
            std::vector<uint8_t>(33, 0xc0)};
        return tx;
    };

    std::vector<std::vector<uint8_t>> p2trScripts{
        std::vector<uint8_t>(34, 0x00)};
    p2trScripts[0][0] = dinero::consensus::OP_1;
    p2trScripts[0][1] = 32;

    std::string reason;
    const Transaction ctv = transactionWithScript(
        {dinero::consensus::OP_CHECKTEMPLATEVERIFY});
    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        ctv, p2trScripts, 19, params, &reason));
    EXPECT_NE(reason.find("OP_CHECKTEMPLATEVERIFY"), std::string::npos);
    EXPECT_TRUE(dinero::policy::IsCovenantRelayStandard(
        ctv, p2trScripts, 20, params));

    const Transaction ccv = transactionWithScript(
        {dinero::consensus::OP_CHECKCONTRACTVERIFY});
    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        ccv, p2trScripts, 29, params, &reason));
    EXPECT_TRUE(dinero::policy::IsCovenantRelayStandard(
        ccv, p2trScripts, 30, params));

    const Transaction csfs = transactionWithScript(
        {dinero::consensus::OP_CHECKSIGFROMSTACK});
    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        csfs, p2trScripts, UINT32_MAX, params, &reason));

    const Transaction csfsVerify = transactionWithScript(
        {dinero::consensus::OP_CHECKSIGFROMSTACKVERIFY});
    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        csfsVerify, p2trScripts, UINT32_MAX, params, &reason));

    const Transaction txhash = transactionWithScript(
        {dinero::consensus::OP_TXHASH});
    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        txhash, p2trScripts, UINT32_MAX, params, &reason));

    Transaction annexedCtv = ctv;
    annexedCtv.vin[0].witness.push_back({0x50, 0x01});
    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        annexedCtv, p2trScripts, 19, params, &reason));

    // Opcode bytes inside pushed data are not executable and must not trigger
    // policy. Key-path witnesses and ordinary tapscripts remain unaffected.
    const Transaction pushedBytes = transactionWithScript({
        4,
        dinero::consensus::OP_CHECKTEMPLATEVERIFY,
        dinero::consensus::OP_CHECKCONTRACTVERIFY,
        dinero::consensus::OP_TXHASH,
        dinero::consensus::OP_CHECKSIGFROMSTACK});
    EXPECT_TRUE(dinero::policy::IsCovenantRelayStandard(
        pushedBytes, p2trScripts, 1, params));

    Transaction keyPath;
    keyPath.vin.emplace_back();
    keyPath.vin[0].witness = {std::vector<uint8_t>(64, 0x01)};
    EXPECT_TRUE(dinero::policy::IsCovenantRelayStandard(
        keyPath, p2trScripts, 1, params));

    Transaction nonTaprootWitness;
    nonTaprootWitness.vin.emplace_back();
    nonTaprootWitness.vin[0].witness = {
        {dinero::consensus::OP_CHECKCONTRACTVERIFY},
        std::vector<uint8_t>(33, 0xc0)};
    const std::vector<std::vector<uint8_t>> p2wshPrevout{
        std::vector<uint8_t>(34, 0x00)};
    EXPECT_TRUE(dinero::policy::IsCovenantRelayStandard(
        nonTaprootWitness, p2wshPrevout, 1, params));

    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        ctv, {}, 19, params, &reason));
    EXPECT_NE(reason.find("missing spent output scripts"), std::string::npos);

    // Pin the production boundary through the actual mainnet parameters, not
    // only through a synthetic ChainParams fixture. Mempool and block-template
    // policy validate the next candidate block height.
    SelectParams(Chain::MAINNET);
    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        ctv, p2trScripts, 79999, Params(), &reason));
    EXPECT_TRUE(dinero::policy::IsCovenantRelayStandard(
        ctv, p2trScripts, 80000, Params()));
    EXPECT_FALSE(dinero::policy::IsCovenantRelayStandard(
        ccv, p2trScripts, 79999, Params(), &reason));
    EXPECT_TRUE(dinero::policy::IsCovenantRelayStandard(
        ccv, p2trScripts, 80000, Params()));
}

TEST(CovenantActivation, ConsensusChecksumCommitsToEveryCovenantHeight) {
    SelectParams(Chain::MAINNET);
    EXPECT_EQ(
        ConsensusChecksum(Params()),
        "480c4b727fe55dff2c3200adeb45da8014fbacac02572a4dc6ddfa5ed7399d2c");

    ChainParams baseline{};
    const std::string checksum = ConsensusChecksum(baseline);

    auto expect_committed = [&](uint32_t ChainParams::*field) {
        ChainParams changed = baseline;
        changed.*field = 42;
        EXPECT_NE(ConsensusChecksum(changed), checksum);
    };

    expect_committed(&ChainParams::taproot_scriptpath_activation_height);
    expect_committed(&ChainParams::ctv_activation_height);
    expect_committed(&ChainParams::csfs_activation_height);
    expect_committed(&ChainParams::txhash_activation_height);
    expect_committed(&ChainParams::ccv_activation_height);
}

TEST(CovenantActivation, HighLevelValidationUsesSpendHeightNotCoinHeight) {
    SelectParams(Chain::REGTEST);

    Transaction tx;
    TxInput input;
    input.prevout.txid = TxId(dinero::uint256::FromHexUnsafe(
        "0000000000000000000000000000000000000000000000000000000000000042"));
    input.prevout.vout = 0;
    tx.vin.push_back(input);
    tx.vout.emplace_back(
        dinero::AmountUna::Una(9'000),
        std::vector<uint8_t>{dinero::consensus::OP_TRUE});

    const auto template_hash =
        dinero::consensus::ComputeCTVHash(tx, 0);
    std::vector<uint8_t> script{32};
    script.insert(
        script.end(), template_hash.begin(), template_hash.end());
    script.push_back(
        static_cast<uint8_t>(
            dinero::consensus::OP_CHECKTEMPLATEVERIFY));

    const auto leaf_hash =
        dinero::consensus::TapLeafHash(0xc0, script);
    ASSERT_EQ(leaf_hash.size(), 32U);
    std::array<uint8_t, 32> merkle_root{};
    std::copy(
        leaf_hash.begin(), leaf_hash.end(), merkle_root.begin());

    // A deterministic NUMS-style internal key is sufficient for this
    // activation-path test; no key-path secret is used.
    ContractState key_seed{};
    key_seed.stateHash[31] = 1;
    std::array<uint8_t, 32> internal_key{};
    ASSERT_TRUE(dinero::consensus::DeriveContractInternalKey(
        key_seed, internal_key));

    std::vector<uint8_t> spent_script;
    uint8_t parity = 0;
    ASSERT_TRUE(dinero::consensus::ComputeContractOutputScript(
        key_seed, merkle_root, spent_script, &parity));

    std::vector<uint8_t> control_block{
        static_cast<uint8_t>(0xc0 | parity)};
    control_block.insert(
        control_block.end(), internal_key.begin(), internal_key.end());
    tx.vin[0].witness = {script, control_block};

    const OutPoint outpoint{tx.vin[0].prevout.txid, 0};
    OneCoinProvider provider(
        outpoint,
        UTXOEntry(
            dinero::AmountUna::Una(10'000), spent_script,
            1 /* coin creation height */, false));

    // Script path and CTV activate at validation height 20 even though the
    // spent coin was created before activation.
    const auto result =
        TransactionValidator::ValidateTransaction(tx, &provider, 20);
    EXPECT_TRUE(result.valid) << result.error;
    EXPECT_EQ(result.total_fee, dinero::AmountUna::Una(1'000));
}

TEST(CovenantActivation, MainnetValidationChangesExactlyAtHeight80000) {
    SelectParams(Chain::MAINNET);

    Transaction tx;
    TxInput input;
    input.prevout.txid = TxId(dinero::uint256::FromHexUnsafe(
        "0000000000000000000000000000000000000000000000000000000000000080"));
    input.prevout.vout = 0;
    tx.vin.push_back(input);
    tx.vout.emplace_back(
        dinero::AmountUna::Una(9'000),
        std::vector<uint8_t>{dinero::consensus::OP_TRUE});

    auto wrong_hash = dinero::consensus::ComputeCTVHash(tx, 0);
    wrong_hash[0] ^= 0x01;
    std::vector<uint8_t> script{32};
    script.insert(script.end(), wrong_hash.begin(), wrong_hash.end());
    script.push_back(static_cast<uint8_t>(
        dinero::consensus::OP_CHECKTEMPLATEVERIFY));

    const auto leaf_hash = dinero::consensus::TapLeafHash(0xc0, script);
    ASSERT_EQ(leaf_hash.size(), 32U);
    std::array<uint8_t, 32> merkle_root{};
    std::copy(leaf_hash.begin(), leaf_hash.end(), merkle_root.begin());

    ContractState key_seed{};
    key_seed.stateHash[31] = 2;
    std::array<uint8_t, 32> internal_key{};
    ASSERT_TRUE(dinero::consensus::DeriveContractInternalKey(
        key_seed, internal_key));

    std::vector<uint8_t> spent_script;
    uint8_t parity = 0;
    ASSERT_TRUE(dinero::consensus::ComputeContractOutputScript(
        key_seed, merkle_root, spent_script, &parity));

    std::vector<uint8_t> control_block{
        static_cast<uint8_t>(0xc0 | parity)};
    control_block.insert(
        control_block.end(), internal_key.begin(), internal_key.end());
    tx.vin[0].witness = {script, control_block};

    const OutPoint outpoint{tx.vin[0].prevout.txid, 0};
    OneCoinProvider provider(
        outpoint,
        UTXOEntry(
            dinero::AmountUna::Una(10'000), spent_script,
            1 /* coin creation height */, false));

    // The deliberately wrong template is accepted while 0xb3 is still NOP4,
    // then rejected by the very first block enforcing CTV.
    const auto before =
        TransactionValidator::ValidateTransaction(tx, &provider, 79999);
    EXPECT_TRUE(before.valid) << before.error;
    const auto active =
        TransactionValidator::ValidateTransaction(tx, &provider, 80000);
    EXPECT_FALSE(active.valid);
}

TEST(CovenantActivation, CtvPrecomputationMatchesCanonicalHashing) {
    Transaction tx;
    for (uint32_t index = 0; index < 32; ++index) {
        TxInput input;
        dinero::uint256 txid;
        txid.data[0] = static_cast<uint8_t>(index + 1);
        input.prevout.txid = TxId(txid);
        input.prevout.vout = index;
        input.sequence = 0xfffffffeU - index;
        if (index == 7) {
            input.scriptSig = {0x01, 0x42};
        }
        tx.vin.push_back(std::move(input));

        tx.vout.emplace_back(
            dinero::AmountUna::Una(1'000 + index),
            std::vector<uint8_t>{
                dinero::consensus::OP_TRUE,
                static_cast<uint8_t>(index)});
    }

    const PrecomputedTransactionData precomputed(tx);
    for (uint32_t index = 0; index < tx.vin.size(); ++index) {
        std::array<uint8_t, 32> direct{};
        std::array<uint8_t, 32> cached{};
        ASSERT_TRUE(dinero::consensus::TryComputeCTVHash(
            tx, index, direct));
        ASSERT_TRUE(dinero::consensus::TryComputeCTVHash(
            tx, index, cached, &precomputed));
        EXPECT_EQ(cached, direct);
    }

    Transaction other = tx;
    const PrecomputedTransactionData wrong_transaction(other);
    std::array<uint8_t, 32> rejected{};
    EXPECT_FALSE(dinero::consensus::TryComputeCTVHash(
        tx, 0, rejected, &wrong_transaction));

    tx.has_explicit_fee = true;
    const PrecomputedTransactionData ineligible(tx);
    EXPECT_FALSE(ineligible.TryComputeCTVHash(0, rejected));
}

TEST(CovenantActivation, LegacyCtvReceivesSharedPrecomputation) {
    SelectParams(Chain::REGTEST);

    Transaction tx;
    tx.vin.emplace_back();
    tx.vout.emplace_back(
        dinero::AmountUna::Una(1'000),
        std::vector<uint8_t>{dinero::consensus::OP_TRUE});

    const auto template_hash = dinero::consensus::ComputeCTVHash(tx, 0);
    std::vector<uint8_t> covenant_script{32};
    covenant_script.insert(
        covenant_script.end(), template_hash.begin(), template_hash.end());
    covenant_script.push_back(
        static_cast<uint8_t>(
            dinero::consensus::OP_CHECKTEMPLATEVERIFY));

    const PrecomputedTransactionData matching(tx);
    EXPECT_TRUE(dinero::consensus::verifyScript(
        {}, covenant_script, {}, tx, 0, 1'000,
        {}, {}, {}, {}, 20, &matching));

    // A precomputation object is transaction-bound. This intentionally
    // mismatched object proves that the canonical legacy/P2SH entry point
    // forwards the shared object into OP_CTV instead of silently rebuilding
    // transaction-wide hashes for every execution.
    Transaction other = tx;
    other.lockTime = 1;
    const PrecomputedTransactionData mismatched(other);
    EXPECT_FALSE(dinero::consensus::verifyScript(
        {}, covenant_script, {}, tx, 0, 1'000,
        {}, {}, {}, {}, 20, &mismatched));
}

} // namespace
