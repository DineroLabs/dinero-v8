#include "wallet/unsigned_tx_builder.h"
#include "wallet/address.h"  // For Bech32 validation
#include "wallet/coin_selection.h"  // Shared size/fee helpers (keeps builder + selector in lockstep)
#include "wallet/p2mr_address.h"   // Phase 10: P2MR address decode + scriptPubKey build
#include "crypto/hash.h"     // For SHA256 hashing
#include "address/addr_codec.h"  // For Taproot address functions
#include <algorithm>
#include <numeric>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════

BuildResult UnsignedTxBuilder::Build(
    const std::vector<CanonicalWalletUTXO>& selected_utxos,  // Phase M.3: Using dinero::WalletUTXO
    const std::vector<TxOutputRequest>& outputs,
    const BuildOptions& options
) {
    BuildResult result;

    // Validate inputs
    if (selected_utxos.empty()) {
        result.error = "No UTXOs selected for transaction";
        return result;
    }

    if (outputs.empty()) {
        result.error = "No outputs specified";
        return result;
    }

    // Calculate total input value
    uint64_t total_input = std::accumulate(
        selected_utxos.begin(), selected_utxos.end(), uint64_t(0),
        [](uint64_t sum, const CanonicalWalletUTXO& utxo) { return sum + utxo.value.GetUna(); }  // Phase M.3: CanonicalWalletUTXO.value is uint64_t
    );

    // Calculate total output value (requested payments)
    uint64_t total_output = std::accumulate(
        outputs.begin(), outputs.end(), uint64_t(0),
        [](uint64_t sum, const TxOutputRequest& out) { return sum + out.amount; }
    );

    // Phase 10: count P2MR inputs so the fee estimator weighs the ~5 KB
    // ML-DSA-65 witness per P2MR input (vs 66 bytes for a Schnorr Taproot
    // witness). Without this the tx is priced below min-feerate.
    size_t num_p2mr_inputs = 0;
    for (const auto& u : selected_utxos) {
        if (u.spk.size() == 34 && u.spk[0] == 0x53 && u.spk[1] == 0x20) {
            ++num_p2mr_inputs;
        }
    }

    // Estimate fee (start with num_outputs, may add change later)
    size_t estimated_size = EstimateTransactionSize(
        selected_utxos.size(), outputs.size(), num_p2mr_inputs);
    uint64_t estimated_fee = CalculateFee(estimated_size, options.fee_rate);

    // Calculate potential change
    if (total_input < total_output + estimated_fee) {
        result.error = "Insufficient funds: have " + std::to_string(total_input) +
                       ", need " + std::to_string(total_output + estimated_fee);
        return result;
    }

    uint64_t potential_change = total_input - total_output - estimated_fee;

    // Decide if we need a change output
    bool create_change = ShouldCreateChangeOutput(potential_change, options.dust_threshold);

    // If creating change, recalculate fee with extra output
    std::string change_address;
    uint64_t change_amount = 0;
    uint64_t final_fee = estimated_fee;

    if (create_change) {
        // Use custom change address or require one
        if (options.change_address.empty()) {
            result.error = "Change needed but no change address provided";
            return result;
        }

        change_address = options.change_address;

        // Recalculate with change output
        size_t size_with_change = EstimateTransactionSize(
            selected_utxos.size(), outputs.size() + 1, num_p2mr_inputs);
        final_fee = CalculateFee(size_with_change, options.fee_rate);

        // Recalculate change with updated fee
        if (total_input < total_output + final_fee) {
            result.error = "Insufficient funds after fee adjustment";
            return result;
        }

        change_amount = total_input - total_output - final_fee;

        // Verify change is still above dust after fee adjustment
        if (change_amount < options.dust_threshold) {
            // Change became dust, add to fee instead
            final_fee += change_amount;
            change_amount = 0;
            create_change = false;
        }
    } else {
        // No change output, add leftover to fee
        final_fee = potential_change;
    }

    // Build transaction structure
    Transaction tx;
    tx.version = 2;  // Version 2 (BIP68 relative lock-time)
    tx.lockTime = options.locktime;

    // Determine sequence number (RBF signaling)
    uint32_t sequence = options.enable_rbf ? RBF_SEQUENCE : DEFAULT_SEQUENCE;

    // Add inputs (no scriptSig, no witness - unsigned)
    for (const auto& utxo : selected_utxos) {
        TxInput input;
        input.prevout.txid = TxId(utxo.txid);  // Phase M.4: Wrap uint256 in TxId semantic type
        input.prevout.vout = utxo.vout;
        input.sequence = sequence;
        // scriptSig remains empty (unsigned)
        tx.vin.push_back(input);
    }

    // Add payment outputs
    for (const auto& output : outputs) {
        TxOutput txout;
        txout.value = dinero::AmountUna::Una(output.amount);
        txout.scriptPubKey = AddressToScriptPubKey(output.address);

        if (txout.scriptPubKey.empty()) {
            result.error = "Invalid output address: " + output.address;
            return result;
        }

        tx.vout.push_back(txout);
    }

    // Add change output if needed
    if (create_change) {
        TxOutput change_output;
        change_output.value = dinero::AmountUna::Una(change_amount);
        change_output.scriptPubKey = AddressToScriptPubKey(change_address);

        if (change_output.scriptPubKey.empty()) {
            result.error = "Invalid change address: " + change_address;
            return result;
        }

        tx.vout.push_back(change_output);
    }

    // Populate result
    result.success = true;
    result.unsigned_tx.tx = tx;
    result.unsigned_tx.fee = final_fee;
    result.unsigned_tx.change_amount = change_amount;
    result.unsigned_tx.change_address = change_address;
    result.unsigned_tx.selected_utxos = selected_utxos;
    result.unsigned_tx.signals_rbf = options.enable_rbf;

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Size Estimation (Taproot P2TR / P2MR)
// ═══════════════════════════════════════════════════════════════════════════
//
// The builder and the coin selector MUST agree on size/fee math, otherwise the
// selector picks inputs for one fee but the builder pays another. Both now
// return BIP141 virtual size (Transaction::GetVirtualSize()), matching the
// mempool's fee-rate denominator. Delegating prevents drift.

size_t UnsignedTxBuilder::EstimateTransactionSize(size_t num_inputs,
                                                  size_t num_outputs,
                                                  size_t num_p2mr_inputs) {
    return CoinSelector::EstimateTransactionSize(num_inputs, num_outputs, num_p2mr_inputs);
}

uint64_t UnsignedTxBuilder::CalculateFee(size_t tx_size, uint64_t fee_rate) {
    // tx_size is BIP141 vsize; fee_rate is una/vbyte.
    return CoinSelector::CalculateFee(tx_size, fee_rate);
}

// ═══════════════════════════════════════════════════════════════════════════
// Private Helpers
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> UnsignedTxBuilder::AddressToScriptPubKey(const std::string& address) {
    // Phase 10: try P2MR (witness v3, bech32m) first — DecodeP2MRAddress
    // returns nullopt for non-P2MR addresses, no exception overhead.
    if (auto p2mr = dinero::wallet::DecodeP2MRAddress(address); p2mr.has_value()) {
        return dinero::wallet::BuildP2MRScriptPubKey(p2mr->merkle_root);
    }

    // Try to decode as Taproot (Bech32m, witness v1) first
    try {
        std::vector<uint8_t> witness_program = dinero::DecodeTaprootWitnessProgram(address);
        return dinero::CreateP2TRScriptPubKey(witness_program);
    } catch (const std::exception&) {
        // Not a Taproot address, try legacy formats
    }

    // Fall back to legacy Bech32 v0 validation
    std::string error;
    if (!Address::validateBech32(address, "din", error)) {
        return {};
    }

    // Create P2WPKH scriptPubKey: OP_0 <20-byte-hash>
    std::vector<uint8_t> scriptPubKey;
    scriptPubKey.push_back(0x00);  // OP_0 (witness version 0)
    scriptPubKey.push_back(0x14);  // Push 20 bytes

    // Generate deterministic 20-byte hash from address
    // This ensures same address → same scriptPubKey (deterministic)
    auto addr_hash = din::crypto::SHA256(reinterpret_cast<const uint8_t*>(address.c_str()), address.length());

    // Use first 20 bytes of hash
    scriptPubKey.insert(scriptPubKey.end(), addr_hash.begin(), addr_hash.begin() + 20);

    return scriptPubKey;
}

bool UnsignedTxBuilder::ShouldCreateChangeOutput(uint64_t change_amount, uint64_t dust_threshold) {
    // Change output is created if:
    // 1. Amount >= dust threshold (economically viable)
    // 2. Amount > 0 (has change to return)

    return change_amount >= dust_threshold;
}

} // namespace dinero
