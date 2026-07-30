#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "consensus/covenants.h"
#include "consensus/interfaces/iutxo_provider.h"
#include "consensus/script_interpreter.h"
#include "consensus/transaction_validator.h"
#include "consensus/utxo_entry.h"
#include "primitives/hash_domains.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"

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

TEST(CovenantActivation, ProductionNetworksDoNotActivateUnreviewedOpcodes) {
    SelectParams(Chain::MAINNET);
    EXPECT_EQ(Params().taproot_scriptpath_activation_height, 1U);
    EXPECT_EQ(Params().ctv_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().csfs_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().txhash_activation_height, UINT32_MAX);
    EXPECT_EQ(Params().ccv_activation_height, UINT32_MAX);

    SelectParams(Chain::TESTNET);
    EXPECT_EQ(Params().taproot_scriptpath_activation_height, 200U);
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

TEST(CovenantActivation, ConsensusChecksumCommitsToEveryCovenantHeight) {
    SelectParams(Chain::MAINNET);
    EXPECT_EQ(
        ConsensusChecksum(Params()),
        "2f6946c7767d4abab61bc609ce5894a5afe7828685f80ebca7318a6d4715c7f4");

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

} // namespace
