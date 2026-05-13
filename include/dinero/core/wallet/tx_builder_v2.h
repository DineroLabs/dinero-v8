#pragma once
#include "wallet/tx_builder_iface.h"
#include "wallet/psbt.h"
#include "wallet/coin_selection.h"
#include "wallet/wallet_iface.h"
#include "wallet/tx_size_estimator.h"
#include "wallet/address_validator.h"
#include <memory>

namespace din {

// Forward declarations
struct IChainView;
struct IFeeEstimator;
struct IWalletDB;
struct IKeyStore;

/**
 * @brief Production V2 transaction builder using clean interfaces
 * 
 * This is the permanent implementation that replaces the legacy adapter.
 * Uses V2 coin selection, deterministic size estimation, and PSBT creation.
 */
class TxBuilderV2 : public ITxBuilder {
public:
    /**
     * @brief Constructor with dependency injection
     * 
     * @param keystore Key storage for signing
     * @param chain_view Blockchain state access
     * @param fee_estimator Fee rate estimation
     * @param wallet_db Wallet database access
     */
    TxBuilderV2(
        std::shared_ptr<IKeyStore> keystore,
        std::shared_ptr<IChainView> chain_view,
        std::shared_ptr<IFeeEstimator> fee_estimator,
        std::shared_ptr<IWalletDB> wallet_db
    );
    
    /**
     * @brief Create funded PSBT from transaction request
     * 
     * Full V2 implementation:
     * 1. Validates all addresses with network checks
     * 2. Selects optimal UTXO set using Branch-and-Bound + greedy
     * 3. Calculates deterministic transaction size
     * 4. Adds change output if needed (dust rules applied)
     * 5. Creates complete PSBT with proper UTXO attachments
     * 6. Adds BIP32 derivation paths for hardware wallet compatibility
     * 
     * @param req Transaction build request
     * @param err Output error code
     * @return PSBT result with fee and change information, or nullopt on error
     */
    std::optional<TxBuildResult> create(const TxBuildRequest& req, TxBuildErr& err) override;
    
    /**
     * @brief Estimate transaction size in virtual bytes
     * 
     * Uses deterministic size estimator with real address type detection.
     */
    uint64_t estimateSize(const TxBuildRequest& req) override;

private:
    std::shared_ptr<IKeyStore> keystore_;
    std::shared_ptr<IChainView> chain_view_;
    std::shared_ptr<IFeeEstimator> fee_estimator_;
    std::shared_ptr<IWalletDB> wallet_db_;
    
    // V2 components
    std::unique_ptr<CoinSelector> coin_selector_;
    
    /**
     * @brief Validate transaction request
     */
    bool validateRequest(const TxBuildRequest& req, TxBuildErr& err);
    
    /**
     * @brief Select UTXOs for transaction
     */
    std::optional<CoinSelectionResult> selectCoins(
        const TxBuildRequest& req, 
        int64_t target_value,
        TxBuildErr& err
    );
    
    /**
     * @brief Create change output if needed
     */
    std::optional<TxOut> createChangeOutput(
        const CoinSelectionResult& selection,
        int64_t target_value,
        int64_t fee,
        TxBuildErr& err
    );
    
    /**
     * @brief Build PSBT from selected coins and outputs
     */
    std::optional<Psbt> buildPsbt(
        const TxBuildRequest& req,
        const CoinSelectionResult& selection,
        const std::vector<TxOut>& all_outputs,
        TxBuildErr& err
    );
    
    /**
     * @brief Add UTXO metadata to PSBT input
     */
    void addUtxoMetadata(Psbt& psbt, size_t input_idx, const SelectableUTXO& utxo);
    
    /**
     * @brief Get dust threshold for output type
     */
    int64_t getDustThreshold(AddressValidator::AddressType type);
    
    /**
     * @brief Anti-fee-sniping locktime
     */
    uint32_t getAntiFeeSniping();
};

} // namespace din
