#include "utxo_manager.h"
#include <algorithm>
#include <stdexcept>
#include <sstream>

namespace dinero {
namespace wallet {
namespace reference {

UTXOManager::UTXOManager(Database* database)
    : database_(database) {
    if (!database_) {
        throw std::invalid_argument("Database pointer cannot be null");
    }
}

UTXOManager::~UTXOManager() = default;

void UTXOManager::AddUTXO(const UTXO& utxo) {
    Database::UTXORow row;
    row.txid = utxo.txid;
    row.vout = utxo.vout;
    row.amount = utxo.amount;
    row.script_pubkey = utxo.script_pubkey;
    row.height = utxo.height;
    row.is_coinbase = utxo.is_coinbase;

    database_->InsertUTXO(row);
}

void UTXOManager::RemoveUTXO(const std::string& txid, uint32_t vout,
                            const std::string& spent_in_txid, uint32_t spent_at_height) {
    database_->MarkUTXOSpent(txid, vout, spent_in_txid, spent_at_height);
}

std::vector<UTXO> UTXOManager::GetUnspentUTXOs(uint32_t min_confirmations, uint32_t current_height) const {
    std::vector<UTXO> result;

    // Get all UTXOs from database (already sorted by txid, vout)
    auto db_utxos = database_->GetAllUTXOs();

    for (const auto& db_utxo : db_utxos) {
        UTXO utxo;
        utxo.txid = db_utxo.txid;
        utxo.vout = db_utxo.vout;
        utxo.amount = db_utxo.amount;
        utxo.script_pubkey = db_utxo.script_pubkey;
        utxo.height = db_utxo.height;
        utxo.is_coinbase = db_utxo.is_coinbase;

        // Check confirmations
        uint32_t confirmations = GetConfirmations(utxo.height, current_height);
        if (confirmations < min_confirmations) {
            continue; // Skip unconfirmed or insufficiently confirmed
        }

        // Check coinbase maturity
        if (utxo.is_coinbase && !IsCoinbaseMature(utxo, current_height)) {
            continue; // Skip immature coinbase
        }

        result.push_back(utxo);
    }

    // Ensure deterministic sorting (should already be sorted by database)
    std::sort(result.begin(), result.end());

    return result;
}

std::vector<UTXO> UTXOManager::SelectUTXOs(
    uint64_t target_amount,
    uint64_t fee,
    uint32_t min_confirmations,
    uint32_t current_height
) const {
    // Get all available UTXOs (already sorted deterministically)
    auto available_utxos = GetUnspentUTXOs(min_confirmations, current_height);

    std::vector<UTXO> selected;
    uint64_t selected_amount = 0;
    uint64_t required_amount = target_amount + fee;

    // Greedy selection: take UTXOs from lowest to highest until we have enough
    for (const auto& utxo : available_utxos) {
        selected.push_back(utxo);
        selected_amount += utxo.amount;

        if (selected_amount >= required_amount) {
            return selected; // Success
        }
    }

    // Insufficient funds
    std::ostringstream oss;
    oss << "Insufficient funds: need " << required_amount
        << " una, have " << selected_amount << " una";
    throw std::runtime_error(oss.str());
}

Balance UTXOManager::CalculateBalance(uint32_t min_confirmations, uint32_t current_height) const {
    Balance balance = {0, 0, 0, 0};

    auto db_utxos = database_->GetAllUTXOs();

    for (const auto& db_utxo : db_utxos) {
        uint32_t confirmations = GetConfirmations(db_utxo.height, current_height);

        if (db_utxo.is_coinbase && confirmations <= 100) {
            // Immature coinbase (needs >100 confirmations = 101+ to be spendable)
            balance.immature += db_utxo.amount;
        } else if (confirmations == 0) {
            // Unconfirmed
            balance.unconfirmed += db_utxo.amount;
        } else if (confirmations >= min_confirmations) {
            // Confirmed
            balance.confirmed += db_utxo.amount;
        } else {
            // Has confirmations but not enough
            balance.unconfirmed += db_utxo.amount;
        }
    }

    balance.total = balance.confirmed + balance.unconfirmed + balance.immature;

    return balance;
}

bool UTXOManager::IsSpent(const std::string& txid, uint32_t vout) const {
    return database_->IsUTXOSpent(txid, vout);
}

size_t UTXOManager::GetUTXOCount() const {
    return database_->GetAllUTXOs().size();
}

} // namespace reference
} // namespace wallet
} // namespace dinero
