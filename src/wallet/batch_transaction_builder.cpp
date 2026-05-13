#include "wallet/batch_transaction_builder.h"
#include <numeric>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// Batch Transaction Building
// ═══════════════════════════════════════════════════════════════════════════

BuildResult BatchTransactionBuilder::buildBatch(
    const std::vector<CanonicalWalletUTXO>& selected_utxos,  // Phase M.3: Using dinero::WalletUTXO
    const std::vector<BatchPayment>& payments,
    const BuildOptions& options
) {
    // Validate batch is non-empty
    if (payments.empty()) {
        BuildResult result;
        result.error = "Batch transaction requires at least one payment";
        return result;
    }

    // Convert batch payments to transaction output requests
    // This is the ONLY thing this class does - everything else delegates
    // to existing, battle-tested transaction building logic
    std::vector<TxOutputRequest> outputs;
    outputs.reserve(payments.size());

    for (const auto& payment : payments) {
        // Validate payment
        if (payment.amount == 0) {
            BuildResult result;
            result.error = "Payment amount cannot be zero for address: " + payment.address;
            return result;
        }

        if (payment.address.empty()) {
            BuildResult result;
            result.error = "Payment address cannot be empty";
            return result;
        }

        // Convert to TxOutputRequest
        outputs.emplace_back(payment.address, payment.amount);
    }

    // Delegate to existing unsigned transaction builder
    // All the hard work happens here:
    // - Fee calculation (based on total tx size)
    // - Change output creation (if needed)
    // - Dust handling
    // - RBF signaling
    // - Transaction structure validation
    return UnsignedTxBuilder::Build(selected_utxos, outputs, options);
}

// ═══════════════════════════════════════════════════════════════════════════
// Helper Functions
// ═══════════════════════════════════════════════════════════════════════════

uint64_t BatchTransactionBuilder::calculateTotalAmount(const std::vector<BatchPayment>& payments) {
    return std::accumulate(
        payments.begin(), payments.end(), uint64_t(0),
        [](uint64_t sum, const BatchPayment& payment) {
            return sum + payment.amount;
        }
    );
}

} // namespace dinero
