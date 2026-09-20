#include "daemon/block_acceptor.h"
#include "consensus/block_validation.h"
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/merkle_root.h"
#include "consensus/subsidy.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/witness_commitment.h"
#include "wallet/canonical_wallet_utxo.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace dinero {
class BlockAcceptorFeeTestAccess {
public:
    static bool Check(const Block& block, uint32_t height, std::string& error) {
        ParsedBlock parsed{};
        parsed.txCount = block.vtx.size();
        for (const auto& tx : block.vtx) {
            const auto bytes = tx.Serialize(TxSerializationMode::WithWitness);
            std::string hex;
            for (uint8_t b : bytes) {
                hex += "0123456789abcdef"[b >> 4];
                hex += "0123456789abcdef"[b & 15];
            }
            parsed.transactions.push_back(std::move(hex));
        }
        return BlockAcceptor::ValidateContextual(parsed, height, error);
    }
};
}

using namespace dinero;
using namespace dinero::consensus;
namespace {
void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
constexpr uint32_t HEIGHT = 106;
constexpr uint64_t COIN = 100000000;

struct Fixture {
    ConsensusUTXOSet coins;
    Block block;
    OutPoint funding{TxId(uint256::FromHexUnsafe(std::string(64, '1'))), 0};
    UtreexoHash before;

    Fixture(uint64_t fee, uint64_t excess = 0, bool split = false) {
        // Isolate fee accounting from DNRS; the live daemon integration keeps
        // DNRS enabled. Signatures, maturity and Utreexo checks stay enabled here.
        SelectParams(Chain::REGTEST);
        MutableParams().state_commitment_activation_height = UINT32_MAX;
        std::array<uint8_t, 32> secret{}, pubkey{};
        secret.fill(3);
        int parity = 0;
        Require(TaprootKeys::DeriveXOnlyPubkey(secret, pubkey, parity), "derive test key");
        std::vector<uint8_t> script{0x51, 0x20};
        script.insert(script.end(), pubkey.begin(), pubkey.end());
        UTXOEntry coin(AmountUna::Una(100 * COIN), script, 1, true);
        Require(coins.AddCoin(funding, coin), "seed funding coin");
        const auto leaf = HashUTXOForCreationHeight(funding.txid.AsUint256(), 0,
            coin.value.GetUna(), script, coin.height, coin.isCoinbase);
        Require(coins.GetForest().add(leaf) != UINT64_MAX, "seed funding leaf");
        before = coins.GetForest().getCommitment();

        Transaction spend;
        spend.version = 2;
        spend.witness_version = 1;
        TxInput input;
        input.prevout.txid = funding.txid; input.prevout.vout = funding.vout;
        input.sequence = 0xfffffffe;
        spend.vin.push_back(input);
        TxOutput output;
        output.value = AmountUna::Una(100 * COIN - fee);
        output.scriptPubKey = script;
        spend.vout.push_back(output);
        CanonicalWalletUTXO wallet;
        wallet.txid = funding.txid.AsUint256(); wallet.vout = 0;
        wallet.value = coin.value; wallet.spk = script;
        wallet.height = 1; wallet.is_coinbase = true;
        const auto sighash = TaprootTxSigner::ComputeTaprootSighash(
            spend, 0, {wallet}, TaprootTxSigner::SIGHASH_DEFAULT);
        Require(sighash.size() == 32, "compute signature hash");
        std::array<uint8_t, 32> msg{};
        std::memcpy(msg.data(), sighash.data(), msg.size());
        std::array<uint8_t, 64> sig{};
        Require(TaprootKeys::SignSchnorr(sig, msg, secret), "sign spend");
        spend.vin[0].witness.emplace_back(sig.begin(), sig.end());

        Transaction cb;
        cb.version = 1; cb.witness_version = 1;
        TxInput cb_in;
        cb_in.prevout.txid = TxId(); cb_in.prevout.vout = UINT32_MAX;
        cb_in.scriptSig = {1, HEIGHT}; cb_in.sequence = UINT32_MAX;
        cb.vin.push_back(cb_in);
        output.value = AmountUna::Una(ConsensusSubsidy::GetBlockSubsidy(HEIGHT).GetUna() + fee);
        cb.vout.push_back(output);
        if (split) {
            // First output is legal; only the sum reveals the extra payout.
            output.value = AmountUna::Una(excess);
            cb.vout.push_back(output);
        } else {
            cb.vout[0].value = AmountUna::Una(cb.vout[0].value.GetUna() + excess);
        }
        block.vtx = {cb, spend};
        TxOutput witness;
        witness.value = AmountUna::Zero();
        witness.scriptPubKey = BuildWitnessCommitment(block.vtx);
        block.vtx[0].vout.push_back(witness);
        block.header.version = 1;
        block.header.timestamp = 1772496000 + HEIGHT * 120;
        block.header.difficulty = 0x207fffff;
        block.header.ZeroReserved();
        block.header.merkle_root = ComputeMerkleRoot(block.vtx);

        BlockUtreexoData proof;
        proof.accumulator_root_before = before;
        proof.spend_proof = coins.GetForest().generateBlockProof(
            {leaf}, GetUtreexoProofFormatVersion(HEIGHT));
        proof.spent_outputs.emplace_back(coin.value.GetUna(), script, 1, true);
        block.utreexo = proof;
        BlockValidator validator(&coins);
        std::string error;
        Require(validator.ComputeUtreexoRootPure(block, HEIGHT, block.header.utreexo_root, error), error);
        Require(coins.GetForest().getCommitment() == before, "root computation mutated forest");
    }
};

void Admission(uint64_t fee) {
    Fixture f(fee);
    std::string error;
    Require(BlockAcceptorFeeTestAccess::Check(f.block, HEIGHT, error), error);
}

void ValidateReward(bool stateless, uint64_t excess, bool split) {
    Fixture f(COIN / 10, excess, split);
    BlockValidator validator(&f.coins);
    if (stateless) validator.setValidationMode(ValidationMode::STATELESS);
    BlockUndo undo;
    std::string error;
    const bool accepted = validator.ValidateAndApplyBlock(
        f.block, HEIGHT, f.block.GetHash(), undo, error);
    Require(accepted == (excess == 0), "wrong reward result: " + error);
    if (excess) {
        Require(error.find("Coinbase pays too much") != std::string::npos, error);
        Require(f.coins.GetForest().getCommitment() == f.before, "overpayment mutated forest");
        Require(f.coins.HaveCoin(f.funding), "overpayment spent funding coin");
    }
}
}

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"admission_low_fee", [] { Admission(COIN / 100); }},
        {"admission_high_fee", [] { Admission(COIN / 10); }},
        {"admission_large_fee", [] { Admission(90 * COIN); }},
        {"bip34_still_checked", [] {
            Fixture f(COIN / 10); std::string error;
            Require(!BlockAcceptorFeeTestAccess::Check(f.block, HEIGHT + 1, error), "wrong height accepted");
            Require(error.find("BIP34 height mismatch") != std::string::npos, error);
        }},
        {"stateful_actual_fee", [] { ValidateReward(false, 0, false); }},
        {"stateful_overpay", [] { ValidateReward(false, 1, false); }},
        {"stateful_split_overpay", [] { ValidateReward(false, 1, true); }},
        {"stateless_actual_fee", [] { ValidateReward(true, 0, false); }},
        {"stateless_overpay", [] { ValidateReward(true, 1, false); }},
        {"stateless_split_overpay", [] { ValidateReward(true, 1, true); }},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    std::cout << tests.size() - failures << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
