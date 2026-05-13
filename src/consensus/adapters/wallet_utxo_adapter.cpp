#include "consensus/adapters/wallet_utxo_adapter.h"
#include <stdexcept>

namespace dinero {
namespace consensus {

WalletUTXOAdapter::WalletUTXOAdapter(UTXOIndex* wallet_index)
    : wallet_index_(wallet_index) {
    if (!wallet_index_) {
        throw std::runtime_error("WalletUTXOAdapter: wallet_index cannot be null");
    }
}

std::optional<UTXOEntry> WalletUTXOAdapter::GetUTXO(const OutPoint& outpoint) const {
    auto wallet_utxo = wallet_index_->GetUTXO(outpoint.txid, outpoint.vout);
    if (!wallet_utxo) {
        return std::nullopt;
    }
    return ToUTXOEntry(*wallet_utxo);
}

bool WalletUTXOAdapter::AddUTXO(const OutPoint& outpoint, const UTXOEntry& entry) {
    WalletUTXO wallet_utxo = ToWalletUTXO(outpoint, entry);
    return wallet_index_->AddUTXO(wallet_utxo);
}

bool WalletUTXOAdapter::SpendUTXO(const OutPoint& outpoint, uint32_t spend_height) {
    return wallet_index_->SpendUTXO(outpoint.txid, outpoint.vout, spend_height);
}

bool WalletUTXOAdapter::DeleteUTXO(const OutPoint& outpoint) {
    return wallet_index_->DeleteUTXO(outpoint.txid, outpoint.vout);
}

bool WalletUTXOAdapter::HasUTXO(const OutPoint& outpoint) const {
    auto utxo = wallet_index_->GetUTXO(outpoint.txid, outpoint.vout);
    return utxo.has_value();
}

// ═══════════════════════════════════════════════════════════════════════════
// Wallet-specific methods (NOT part of IUTXOProvider)
// ═══════════════════════════════════════════════════════════════════════════

std::optional<std::string> WalletUTXOAdapter::IsOurScript(const std::vector<uint8_t>& scriptPubKey) const {
    return wallet_index_->IsOurScript(scriptPubKey);
}

std::vector<WalletUTXO> WalletUTXOAdapter::GetUnspentUTXOs() const {
    return wallet_index_->GetUnspentUTXOs();
}

// ═══════════════════════════════════════════════════════════════════════════
// Type conversion helpers
// ═══════════════════════════════════════════════════════════════════════════

UTXOEntry WalletUTXOAdapter::ToUTXOEntry(const WalletUTXO& utxo) {
    return UTXOEntry(
        utxo.value,
        utxo.spk,
        static_cast<uint32_t>(utxo.height),
        utxo.is_coinbase,
        utxo.is_confidential,
        utxo.commitment
    );
}

WalletUTXO WalletUTXOAdapter::ToWalletUTXO(const OutPoint& outpoint, const UTXOEntry& entry) {
    WalletUTXO utxo;
    utxo.txid = outpoint.txid;
    utxo.vout = outpoint.vout;
    utxo.value = entry.value;
    utxo.spk = entry.scriptPubKey;
    utxo.path = "";  // Consensus UTXO doesn't track derivation path
    utxo.height = static_cast<int>(entry.height);
    utxo.spend_height = std::nullopt;  // Not spent
    utxo.is_coinbase = entry.isCoinbase;
    utxo.is_confidential = entry.is_confidential;
    utxo.commitment = entry.commitment;
    return utxo;
}

} // namespace consensus
} // namespace dinero
