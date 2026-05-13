#include "wallet/tx_builder_v2.h"
#include "wallet/wallet_iface.h"
#include "wallet/unsigned_tx_builder.h"
#include "wallet/psbt.h"
#include "wallet/wallet_metrics.h"
#include "consensus/coin_type.h"
#include <algorithm>
#include <stdexcept>

namespace din {

TxBuilderV2::TxBuilderV2(
    std::shared_ptr<IKeyStore> keystore,
    std::shared_ptr<IChainView> chain_view,
    std::shared_ptr<IFeeEstimator> fee_estimator,
    std::shared_ptr<IWalletDB> wallet_db
) : keystore_(std::move(keystore)),
    chain_view_(std::move(chain_view)),
    fee_estimator_(std::move(fee_estimator)),
    wallet_db_(std::move(wallet_db)) {
    
    // Initialize V2 coin selector
    coin_selector_ = std::make_unique<CoinSelector>();
}

std::optional<TxBuildResult> TxBuilderV2::create(const TxBuildRequest& req, TxBuildErr& err) {
    try {
        // Increment request counter
        WalletMetrics::incrementTxBuilderRequest();
        
        // Step 1: Validate request
        if (!validateRequest(req, err)) {
            WalletMetrics::incrementTxBuilderFail();
            return std::nullopt;
        }
        
        // Step 2: Calculate target value
        int64_t target_value = 0;
        for (const auto& output : req.vout) {
            target_value += output.value;
        }
        
        // Step 3: Estimate size for fee calculation
        uint64_t estimated_size = estimateSize(req);
        int64_t estimated_fee = static_cast<int64_t>(estimated_size * req.fee_rate_una_vb);
        
        // Step 4: Select coins
        auto selection = selectCoins(req, target_value + estimated_fee, err);
        if (!selection) {
            return std::nullopt;
        }
        
        // Step 5: Refine fee calculation with actual selection
        std::vector<TxSizeEstimator::InputSpec> input_specs;
        for (const auto& utxo : selection->selected_utxos) {
            // Determine input type from UTXO (simplified for now)
            input_specs.emplace_back(TxSizeEstimator::InputType::P2WPKH);
        }
        
        std::vector<TxSizeEstimator::OutputSpec> output_specs;
        for (const auto& output : req.vout) {
            auto addr_result = AddressValidator::validate(output.address);
            TxSizeEstimator::OutputType output_type = TxSizeEstimator::OutputType::P2WPKH;
            
            switch (addr_result.type) {
                case AddressValidator::AddressType::P2WPKH:
                    output_type = TxSizeEstimator::OutputType::P2WPKH;
                    break;
                case AddressValidator::AddressType::P2PKH:
                    output_type = TxSizeEstimator::OutputType::P2PKH;
                    break;
                default:
                    output_type = TxSizeEstimator::OutputType::P2WPKH; // Default
                    break;
            }
            
            output_specs.emplace_back(output_type, output.value);
        }
        
        // Add potential change output
        if (selection->change_value > 0) {
            output_specs.emplace_back(TxSizeEstimator::OutputType::P2WPKH, selection->change_value);
        }
        
        uint64_t actual_size = TxSizeEstimator::estimateVBytes(input_specs, output_specs);
        int64_t actual_fee = static_cast<int64_t>(actual_size * req.fee_rate_una_vb);
        
        // Step 6: Create change output if needed
        std::vector<TxOut> all_outputs;
        for (const auto& output : req.vout) {
            all_outputs.push_back({output.address, output.value});
        }
        
        int64_t change_amount = selection->selected_value - target_value - actual_fee;
        std::optional<TxOut> change_output;
        
        if (change_amount > 0) {
            change_output = createChangeOutput(*selection, target_value, actual_fee, err);
            if (!change_output && err != TxBuildErr::InternalError) {
                return std::nullopt;
            }
            if (change_output) {
                all_outputs.push_back(*change_output);
            }
        }
        
        // Step 7: Build PSBT
        auto psbt = buildPsbt(req, *selection, all_outputs, err);
        if (!psbt) {
            return std::nullopt;
        }
        
        // Step 8: Serialize PSBT
        auto psbt_bytes = serialize(*psbt);
        std::string psbt_b64 = to_base64(psbt_bytes);
        
        // Step 9: Build result
        TxBuildResult result;
        result.psbt_base64 = psbt_b64;
        result.fee_paid = actual_fee;
        result.change_output_value = change_output ? change_output->value : 0;
        
        // Increment success counter and PSBT created
        WalletMetrics::incrementTxBuilderSuccess();
        WalletMetrics::incrementPsbtCreated();
        
        return result;
        
    } catch (const std::exception&) {
        WalletMetrics::incrementTxBuilderFail();
        err = TxBuildErr::InternalError;
        return std::nullopt;
    }
}

uint64_t TxBuilderV2::estimateSize(const TxBuildRequest& req) {
    // Use V2 size estimator with real address type detection
    std::vector<TxSizeEstimator::InputSpec> input_specs;
    std::vector<TxSizeEstimator::OutputSpec> output_specs;
    
    // Estimate inputs (assume P2WPKH for now - would use UTXO data in real implementation)
    size_t estimated_inputs = req.vin.empty() ? 2 : req.vin.size();
    for (size_t i = 0; i < estimated_inputs; ++i) {
        input_specs.emplace_back(TxSizeEstimator::InputType::P2WPKH);
    }
    
    // Real outputs with address type detection
    for (const auto& output : req.vout) {
        auto addr_result = AddressValidator::validate(output.address);
        TxSizeEstimator::OutputType output_type = TxSizeEstimator::OutputType::P2WPKH;
        
        switch (addr_result.type) {
            case AddressValidator::AddressType::P2WPKH:
                output_type = TxSizeEstimator::OutputType::P2WPKH;
                break;
            case AddressValidator::AddressType::P2PKH:
                output_type = TxSizeEstimator::OutputType::P2PKH;
                break;
            default:
                output_type = TxSizeEstimator::OutputType::P2WPKH;
                break;
        }
        
        output_specs.emplace_back(output_type, output.value);
    }
    
    // Add change output estimate
    output_specs.emplace_back(TxSizeEstimator::OutputType::P2WPKH, 0);
    
    return TxSizeEstimator::estimateVBytes(input_specs, output_specs);
}

bool TxBuilderV2::validateRequest(const TxBuildRequest& req, TxBuildErr& err) {
    // Validate fee rate
    if (req.fee_rate_una_vb <= 0) {
        err = TxBuildErr::InternalError;
        return false;
    }
    
    // Validate outputs
    if (req.vout.empty()) {
        err = TxBuildErr::InternalError;
        return false;
    }
    
    for (const auto& output : req.vout) {
        // V2 address validation with network checks
        if (!AddressValidator::isValid(output.address, AddressValidator::Network::Mainnet)) {
            err = TxBuildErr::InvalidAddress;
            return false;
        }
        
        // Check for dust
        auto addr_result = AddressValidator::validate(output.address);
        int64_t dust_threshold = getDustThreshold(addr_result.type);
        if (output.value < dust_threshold) {
            err = TxBuildErr::DustOutput;
            return false;
        }
    }
    
    return true;
}

std::optional<CoinSelectionResult> TxBuilderV2::selectCoins(
    const TxBuildRequest& req,
    int64_t target_value,
    TxBuildErr& err
) {
    // Get UTXOs from wallet_db_ (basic implementation)
    // In a full implementation, this would query the actual wallet database
    std::vector<SelectableUTXO> available_utxos;
    
    // Test UTXO with sufficient value
    SelectableUTXO test_utxo(
        "0101010101010101010101010101010101010101010101010101010101010101", // txid
        0, // vout
        target_value + 50000, // value (extra for fee)
        6, // confirmations
        true, // spendable
        1 // address_type (P2WPKH)
    );
    available_utxos.push_back(test_utxo);
    
    // Create coin selection request
    CoinSelectionRequest coin_req;
    coin_req.target_value = target_value;
    coin_req.feerate_una_vb = req.fee_rate_una_vb;
    coin_req.min_relay_fee_una_vb = 1.0;
    coin_req.change_output_type = 1; // P2WPKH
    coin_req.enable_rbf = req.replace_by_fee;
    coin_req.current_height = 850000; // Mock height
    
    // Use V2 coin selection
    auto result = coin_selector_->selectCoins(available_utxos, coin_req);
    
    if (!result) {
        err = TxBuildErr::InsufficientFunds;
        return std::nullopt;
    }
    
    return result;
}

std::optional<TxOut> TxBuilderV2::createChangeOutput(
    const CoinSelectionResult& selection,
    int64_t target_value,
    int64_t fee,
    TxBuildErr& err
) {
    int64_t change_amount = selection.selected_value - target_value - fee;
    
    if (change_amount <= 0) {
        return std::nullopt;
    }
    
    // Check dust threshold for change
    int64_t dust_threshold = getDustThreshold(AddressValidator::AddressType::P2WPKH);
    if (change_amount < dust_threshold) {
        // Don't create dust change - fee increases instead
        return std::nullopt;
    }
    
    // TODO: Get real change address from wallet_db_
    // For now, use mock change address
    std::string change_address = "din1qgxq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5pq5";
    
    return TxOut{change_address, change_amount};
}

std::optional<Psbt> TxBuilderV2::buildPsbt(
    const TxBuildRequest& req,
    const CoinSelectionResult& selection,
    const std::vector<TxOut>& all_outputs,
    TxBuildErr& err
) {
    try {
        // Build unsigned transaction
        std::vector<UnsignedTxBuilder::TxInput> tx_inputs;
        std::vector<UnsignedTxBuilder::TxOutput> tx_outputs;
        
        // Add selected UTXOs as inputs
        for (const auto& utxo : selection.selected_utxos) {
            // Convert hex txid to bytes
            std::vector<uint8_t> txid_bytes(32, 0x01); // Mock for now
            tx_inputs.emplace_back(txid_bytes, utxo.vout);
        }
        
        // Add all outputs
        for (const auto& output : all_outputs) {
            // Determine output type and create script
            auto addr_result = AddressValidator::validate(output.address);
            std::vector<uint8_t> script_pubkey;
            
            if (addr_result.type == AddressValidator::AddressType::P2WPKH) {
                // Mock pubkey hash for now
                std::vector<uint8_t> pubkey_hash(20, 0x02);
                script_pubkey = UnsignedTxBuilder::createP2WPKHScript(pubkey_hash);
            } else {
                // Default to P2PKH
                std::vector<uint8_t> pubkey_hash(20, 0x02);
                script_pubkey = UnsignedTxBuilder::createP2PKHScript(pubkey_hash);
            }
            
            tx_outputs.emplace_back(output.value, script_pubkey);
        }
        
        // Set anti-fee-sniping locktime
        uint32_t locktime = getAntiFeeSniping();
        
        // Build unsigned transaction
        auto unsigned_tx = UnsignedTxBuilder::build(tx_inputs, tx_outputs, 2, locktime);
        
        // Create PSBT
        Psbt psbt;
        add_global_unsigned_tx(psbt, unsigned_tx);
        
        // Add input metadata
        for (size_t i = 0; i < selection.selected_utxos.size(); ++i) {
            addUtxoMetadata(psbt, i, selection.selected_utxos[i]);
        }
        
        return psbt;
        
    } catch (const std::exception&) {
        err = TxBuildErr::InternalError;
        return std::nullopt;
    }
}

void TxBuilderV2::addUtxoMetadata(Psbt& psbt, size_t input_idx, const SelectableUTXO& utxo) {
    // Add witness UTXO for SegWit inputs
    std::vector<uint8_t> mock_script = UnsignedTxBuilder::createP2WPKHScript(std::vector<uint8_t>(20, 0x02));
    add_in_witness_utxo(psbt, input_idx, mock_script, utxo.value);
    
    // Add BIP32 derivation path for hardware wallet compatibility
    // TODO: Get real pubkey and derivation path from keystore_
    constexpr uint32_t HARDENED = 0x80000000;
    std::vector<uint8_t> mock_pubkey(33, 0x03);
    std::vector<uint8_t> mock_fingerprint(4, 0x12);
    std::vector<uint32_t> mock_path = {
        HARDENED + 84u,
        HARDENED + dinero::consensus::DINERO_COIN_TYPE,
        HARDENED + 0u,
        0u,
        0u,
    };  // m/84'/coin_type'/0'/0/0
    add_in_bip32_deriv(psbt, input_idx, mock_pubkey, mock_fingerprint, mock_path);
}

int64_t TxBuilderV2::getDustThreshold(AddressValidator::AddressType type) {
    // Bitcoin Core dust thresholds (at 3 sat/vB)
    switch (type) {
        case AddressValidator::AddressType::P2WPKH:
            return 294; // 31 vB * 3 sat/vB * 3 (dust relay factor)
        case AddressValidator::AddressType::P2PKH:
            return 546; // 34 vB * 3 sat/vB * 3 (dust relay factor) + overhead
        default:
            return 546; // Conservative default
    }
}

uint32_t TxBuilderV2::getAntiFeeSniping() {
    // Anti-fee-sniping: set locktime to current height
    // TODO: Get real block height from chain_view_
    // For now, use mock height
    return 850000; // Mock block height
}

} // namespace din
