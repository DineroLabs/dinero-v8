#include "wallet/wallet_mempool_adapter.h"

namespace dinero {
namespace wallet {

// ═══════════════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════════════

WalletMempoolAdapter::WalletMempoolAdapter(IMempoolInterface& mempool)
    : mempool_(mempool) {
}

// ═══════════════════════════════════════════════════════════════════════════
// Policy Testing (Dry-Run Before Signing)
// ═══════════════════════════════════════════════════════════════════════════

TxPolicyResult WalletMempoolAdapter::test(const UnsignedTransaction& unsigned_tx) const {
    // Extract transaction structure from unsigned transaction
    // Mempool only needs the transaction skeleton to validate policy
    // (inputs, outputs, size, fee rate, etc.)
    //
    // NOTE: Witness data is empty in unsigned tx, but that's fine for policy checks
    // Mempool validates:
    // - Size limits
    // - Fee rate
    // - Ancestor/descendant limits
    // - Input availability (not spent)
    // - Output validity (non-negative values)
    //
    // Mempool does NOT validate signatures in TEST mode
    return mempool_.testAcceptTransaction(unsigned_tx.tx);
}

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Submission (After Signing)
// ═══════════════════════════════════════════════════════════════════════════

SubmitResult WalletMempoolAdapter::submit(const SignedTransaction& signed_tx) {
    // Extract fully signed transaction from wallet type
    // Mempool will:
    // 1. Validate signatures (BIP143/BIP341)
    // 2. Check policy rules
    // 3. Add to mempool if accepted
    // 4. Broadcast to peers (if mode=BROADCAST)
    //
    // This is the ONLY place wallet submits transactions to network
    return mempool_.submitTransaction(signed_tx.tx, SubmitMode::BROADCAST);
}

// ═══════════════════════════════════════════════════════════════════════════
// Query Methods (Read-Only)
// ═══════════════════════════════════════════════════════════════════════════

MempoolInfo WalletMempoolAdapter::getMempoolInfo() const {
    return mempool_.getMempoolInfo();
}

bool WalletMempoolAdapter::hasTransaction(const std::string& txid) const {
    return mempool_.hasTransaction(txid);
}

} // namespace wallet
} // namespace dinero
