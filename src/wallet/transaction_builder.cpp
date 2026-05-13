#include "wallet/transaction_builder.h"
#include "wallet/canonical_wallet_utxo.h"  // Phase M.3: Canonical UTXO type
#include "crypto/dinero_crypto_minimal.h"
#include "address/addr_codec.h"            // DecodeAddressAuto, CreateP2TRScriptPubKey
#include "wallet/p2mr_address.h"           // Phase 10 Commit 1: P2MR (witness v3) address decode
#include <iostream>
#include <algorithm>
#include <map>
#include <chrono>

namespace dinero {

// Bech32 constants
const std::string BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

// Taproot (P2TR) vsize estimation
// Base: version(4) + marker(1) + flag(1) + input_count(1) + output_count(1) + locktime(4) = 12
// Per input non-witness: txid(32) + vout(4) + scriptSig_len(1) + sequence(4) = 41
// Per input witness: stack_items(1) + sig_len(1) + schnorr_sig(64) = 66
// Per output: value(8) + spk_len(1) + P2TR_spk(34) = 43  (or P2WPKH: 31)
// Weight = base*4 + witness, vsize = weight/4
static int EstimateVSize(int num_inputs, int num_outputs) {
    int base_size = 12 + num_inputs * 41 + num_outputs * 43;
    int witness_size = num_inputs * 66;
    int weight = base_size * 3 + (base_size + witness_size);
    return (weight + 3) / 4;
}

static int64_t FeeFor(int vsize, double fee_rate) {
    return static_cast<int64_t>(vsize * fee_rate + 0.999);  // Round up
}

// Phase M.3: No conversion needed - CanonicalWalletUTXO is THE type
// BIP143Signer uses CanonicalWalletUTXO directly

// Hex string to bytes
static std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        auto byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::strtol(byte_str.c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

TransactionBuilder::TransactionBuilder(UTXOIndex* utxo_index)
    : utxo_index_(utxo_index) {
}

TransactionBuilder::BuildResult TransactionBuilder::PreviewTransaction(
    const std::vector<Recipient>& recipients,
    const BuildOptions& options
) {
    BuildResult result;

    if (!utxo_index_) {
        result.error = "UTXO index not initialized";
        return result;
    }

    if (recipients.empty()) {
        result.error = "No recipients specified";
        return result;
    }

    // Validate recipients
    int64_t total_output_amount = 0;
    for (const auto& recipient : recipients) {
        if (!ValidateAddress(recipient.address)) {
            result.error = "Invalid recipient address: " + recipient.address;
            return result;
        }
        if (recipient.amount <= 0) {
            result.error = "Invalid amount for address " + recipient.address;
            return result;
        }
        if (recipient.amount < options.dust_threshold) {
            result.error = "Amount below dust threshold for address " + recipient.address;
            return result;
        }
        total_output_amount += recipient.amount;
    }

    // Get available UTXOs: use caller-supplied candidates when provided (already
    // filtered for maturity, chain validity, CT type, etc.), otherwise fall back
    // to fetching from the UTXOIndex and filtering for supported script types.
    std::vector<CanonicalWalletUTXO> utxos_for_selection;

    if (!options.candidate_utxos.empty()) {
        // Caller pre-filtered: accept as-is (P2WPKH/P2TR filtering already done)
        utxos_for_selection = options.candidate_utxos;
    } else {
        auto legacy_utxos = utxo_index_->GetUnspentUTXOs();
        if (legacy_utxos.empty()) {
            result.error = "No unspent UTXOs available";
            return result;
        }

        // Convert legacy UTXOs and filter for supported script types
        for (const auto& legacy_utxo : legacy_utxos) {
            // Phase M.3: Inline conversion from legacy WalletUTXO (utxo_index.h)
            CanonicalWalletUTXO utxo;
            utxo.txid = legacy_utxo.txid.AsUint256();
            utxo.vout = legacy_utxo.vout;
            utxo.value = legacy_utxo.value;
            utxo.spk = legacy_utxo.spk;
            utxo.height = static_cast<uint32_t>(legacy_utxo.height);
            utxo.is_coinbase = legacy_utxo.is_coinbase;
            utxo.path = legacy_utxo.path;

            // Accept P2WPKH (OP_0 <20-byte hash>) and P2TR (OP_1 <32-byte pubkey>)
            bool is_p2wpkh = (utxo.spk.size() == 22 && utxo.spk[0] == 0x00 && utxo.spk[1] == 0x14);
            bool is_p2tr = (utxo.spk.size() == 34 && utxo.spk[0] == 0x51 && utxo.spk[1] == 0x20);
            if (is_p2wpkh || is_p2tr) {
                utxos_for_selection.push_back(utxo);
            }
        }
    }

    if (utxos_for_selection.empty()) {
        result.error = "No spendable UTXOs available";
        return result;
    }

    // Sort UTXOs by value (largest first) for greedy selection
    std::sort(utxos_for_selection.begin(), utxos_for_selection.end(),
              [](const CanonicalWalletUTXO& a, const CanonicalWalletUTXO& b) { return a.value > b.value; });

    // Greedy coin selection
    int num_outputs = static_cast<int>(recipients.size()) + 1;  // +1 for change
    std::vector<CanonicalWalletUTXO> selected;  // Phase M.3: Canonical UTXO type
    int64_t selected_total = 0;

    for (const auto& utxo : utxos_for_selection) {
        selected.push_back(utxo);
        // Phase M.6.2: Extract raw value for accumulation (will use checked arithmetic in M.6.3)
        selected_total += utxo.value.GetUna();

        // Estimate fee with current selection
        int vsize = EstimateVSize(static_cast<int>(selected.size()), num_outputs);
        int64_t fee = FeeFor(vsize, options.fee_rate);

        int64_t needed = total_output_amount + fee;
        if (selected_total >= needed) {
            break;
        }
    }

    // Final fee calculation
    int vsize = EstimateVSize(static_cast<int>(selected.size()), num_outputs);
    int64_t fee = FeeFor(vsize, options.fee_rate);

    if (selected_total < total_output_amount + fee) {
        result.error = "Insufficient funds: have " + std::to_string(selected_total) +
                      " una, need " + std::to_string(total_output_amount + fee) + " una";
        return result;
    }

    // Calculate change
    int64_t change = selected_total - total_output_amount - fee;

    // If change is below dust, add it to fee
    if (change > 0 && change < options.dust_threshold) {
        fee += change;
        change = 0;
        num_outputs--;  // No change output
    }

    result.selected_utxos = selected;
    result.fee = fee;
    result.change_amount = change;

    // Set change address
    if (result.change_amount > 0) {
        result.change_address = options.change_address.value_or(GenerateChangeAddress());
    }

    // Build unsigned transaction
    result.transaction = BuildUnsignedTransaction(
        recipients,
        result.selected_utxos,
        result.change_amount,
        result.change_address,
        options
    );

    // B1a+B1b: Populate timelock/RBF metadata
    result.is_rbf_enabled = options.enable_rbf;
    result.has_absolute_timelock = options.absolute_locktime.has_value();
    result.has_relative_timelocks = !options.relative_locktime_blocks.empty();

    result.success = true;
    return result;
}

TransactionBuilder::BuildResult TransactionBuilder::PreviewTransaction(
    const std::vector<Recipient>& recipients
) {
    BuildOptions default_options;
    return PreviewTransaction(recipients, default_options);
}

TransactionBuilder::BuildResult TransactionBuilder::BuildTransaction(
    const std::vector<Recipient>& recipients,
    const std::map<std::string, std::string>& private_keys,
    const BuildOptions& options
) {
    // First preview the transaction
    auto result = PreviewTransaction(recipients, options);
    if (!result.success) {
        return result;
    }

    // Convert private keys from hex strings to byte vectors
    std::vector<std::vector<uint8_t>> key_bytes;
    std::vector<std::string> required_key_ids;

    for (const auto& utxo : result.selected_utxos) {
        std::string private_key_hex = GetPrivateKeyForUTXO(utxo, private_keys);
        if (private_key_hex.empty()) {
            result.success = false;
            result.error = "Missing private key for UTXO " + utxo.txid.GetHex() + ":" + std::to_string(utxo.vout);
            return result;
        }
        key_bytes.push_back(HexToBytes(private_key_hex));
        required_key_ids.push_back(private_key_hex);
    }

    // Sign each input with the appropriate signer based on script type
    bool sign_success = true;
    for (size_t i = 0; i < result.selected_utxos.size(); i++) {
        if (TaprootTxSigner::IsTaprootUTXO(result.selected_utxos[i])) {
            if (!TaprootTxSigner::SignInput(result.transaction, i, result.selected_utxos, key_bytes[i])) {
                sign_success = false;
                break;
            }
        } else {
            if (!BIP143Signer::SignInput(result.transaction, i, result.selected_utxos[i], key_bytes[i])) {
                sign_success = false;
                break;
            }
        }
    }

    if (!sign_success) {
        result.success = false;
        result.error = "Failed to sign transaction inputs";
        return result;
    }

    result.required_private_keys = required_key_ids;
    std::cout << "INFO: Successfully built and signed transaction with "
              << result.transaction.vin.size() << " inputs" << std::endl;

    return result;
}

TransactionBuilder::BuildResult TransactionBuilder::BuildTransaction(
    const std::vector<Recipient>& recipients,
    const std::map<std::string, std::string>& private_keys
) {
    BuildOptions default_options;
    return BuildTransaction(recipients, private_keys, default_options);
}

int64_t TransactionBuilder::EstimateFee(int num_inputs, int num_outputs, double fee_rate) {
    int vsize = EstimateVSize(num_inputs, num_outputs);
    return FeeFor(vsize, fee_rate);
}

bool TransactionBuilder::ValidateAddress(const std::string& address, const std::string& expected_hrp) {
    // Use canonical address decoder (handles all address types and HRPs)
    // The expected_hrp parameter is ignored - DecodeAddressAuto uses the active network HRP
    (void)expected_hrp;  // Suppress unused parameter warning

    try {
        ParsedAddress parsed = DecodeAddressAuto(address);
        return IsValidDestination(parsed.dest);
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<uint8_t> TransactionBuilder::AddressToScriptPubKey(const std::string& address) {
    // Phase 10 Commit 1: P2MR (BIP-360, witness v3) first. DecodeAddressAuto
    // below doesn't know witness v3 — it would reject the address as
    // UNSUPPORTED_WITNESS_VERSION, and we'd fall through to the empty-return
    // path. Try the P2MR decoder before the generic one so users can send
    // to din1r... / rdin1r... / tdin1r... from the standard send flow.
    if (auto p2mr = dinero::wallet::DecodeP2MRAddress(address); p2mr.has_value()) {
        return dinero::wallet::BuildP2MRScriptPubKey(p2mr->merkle_root);
    }

    // Use canonical address decoder (supports P2WPKH, P2WSH, P2TR, Base58)
    try {
        ParsedAddress parsed = DecodeAddressAuto(address);

        if (!IsValidDestination(parsed.dest)) {
            return {};  // Invalid address
        }

        const auto& program = parsed.dest.pubkey_hash;

        // Build scriptPubKey based on witness program size
        std::vector<uint8_t> script_pubkey;

        if (program.size() == 32) {
            // P2TR (Taproot): OP_1 <32-byte x-only pubkey>
            script_pubkey.reserve(34);
            script_pubkey.push_back(0x51);  // OP_1 (witness version 1)
            script_pubkey.push_back(0x20);  // Push 32 bytes
            script_pubkey.insert(script_pubkey.end(), program.begin(), program.end());
        } else if (program.size() == 20) {
            // P2WPKH: OP_0 <20-byte pubkey hash>
            script_pubkey.reserve(22);
            script_pubkey.push_back(0x00);  // OP_0 (witness version 0)
            script_pubkey.push_back(0x14);  // Push 20 bytes
            script_pubkey.insert(script_pubkey.end(), program.begin(), program.end());
        } else {
            // Unsupported witness program size
            return {};
        }

        return script_pubkey;

    } catch (const std::exception&) {
        return {};  // Decode failed
    }
}

std::string TransactionBuilder::GenerateChangeAddress() {
    // Placeholder implementation - should integrate with HD wallet
    // For now, generate a deterministic address based on current time

    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::string seed = "change_" + std::to_string(timestamp);
    uint8_t hash[32];
    sha256(reinterpret_cast<const uint8_t*>(seed.c_str()), seed.size(), hash);

    // Create a dummy bech32 address
    // In a real implementation, this would use proper HD derivation and bech32 encoding
    std::string change_address = "din1q";

    // Convert first 20 bytes of hash to bech32-like string
    for (int i = 0; i < 20; ++i) {
        change_address += BECH32_CHARSET[hash[i] % 32];
    }

    // Add some padding to make it look more realistic
    change_address += "changexample";

    return change_address;
}

Transaction TransactionBuilder::BuildUnsignedTransaction(
    const std::vector<Recipient>& recipients,
    const std::vector<CanonicalWalletUTXO>& selected_utxos,  // Phase M.3: Canonical UTXO type
    int64_t change_amount,
    const std::string& change_address,
    const BuildOptions& options
) {
    Transaction tx;
    tx.version = 2; // Use version 2 for BIP68/112/113 support
    tx.witness_version = 1; // Taproot - Dinero is Taproot from genesis

    // B1b: Set absolute locktime if specified
    if (options.absolute_locktime.has_value()) {
        tx.lockTime = *options.absolute_locktime;
    } else {
        tx.lockTime = 0;
    }

    // Add inputs
    for (size_t i = 0; i < selected_utxos.size(); i++) {
        const auto& utxo = selected_utxos[i];
        TxInput input;
        input.prevout.txid = TxId(utxo.txid);  // Phase M.4: Wrap uint256 in TxId
        input.prevout.vout = utxo.vout;
        input.scriptSig.clear(); // Empty for P2WPKH (signature goes in witness)

        // B1a+B1b: Compute nSequence based on RBF and timelock options
        auto rel_it = options.relative_locktime_blocks.find(static_cast<uint32_t>(i));
        if (rel_it != options.relative_locktime_blocks.end()) {
            // BIP68: Relative locktime in blocks (bits 0-15, bit 22=0 for height-based)
            // Disable bit (31) must be 0 for relative lock to be enforced
            input.sequence = rel_it->second & 0x0000FFFF;
        } else if (!options.enable_rbf) {
            // Final sequence: disables both RBF and relative timelocks
            input.sequence = 0xffffffff;
        } else {
            // Default: RBF enabled, no relative lock
            input.sequence = 0xfffffffe;
        }

        tx.vin.push_back(input);
    }

    // Add recipient outputs
    for (const auto& recipient : recipients) {
        TxOutput output;
        // Phase M.6.2: Wrap raw value in AmountUna
        output.value = dinero::AmountUna::Una(static_cast<uint64_t>(recipient.amount));
        output.scriptPubKey = AddressToScriptPubKey(recipient.address);
        tx.vout.push_back(output);
    }

    // Add change output if needed
    if (change_amount > 0) {
        TxOutput change_output;
        // Phase M.6.2: Wrap raw value in AmountUna
        change_output.value = dinero::AmountUna::Una(static_cast<uint64_t>(change_amount));
        change_output.scriptPubKey = AddressToScriptPubKey(change_address);
        tx.vout.push_back(change_output);
    }

    return tx;
}

std::string TransactionBuilder::GetPrivateKeyForUTXO(
    const CanonicalWalletUTXO& utxo,  // Phase M.3: Canonical UTXO type
    const std::map<std::string, std::string>& private_keys
) {
    // Try to find private key by derivation path
    auto it = private_keys.find(utxo.path);  // Phase M.3: Use .path field
    if (it != private_keys.end()) {
        return it->second;
    }

    // Try to find by UTXO identifier
    std::string utxo_key = utxo.GetOutpointString();  // Phase M.3: Use helper method
    it = private_keys.find(utxo_key);
    if (it != private_keys.end()) {
        return it->second;
    }

    // If we have only one private key, use it (for testing)
    if (private_keys.size() == 1) {
        return private_keys.begin()->second;
    }

    return "";
}

} // namespace dinero
