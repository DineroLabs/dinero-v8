#pragma once

#include "contracts/escrow_contract.h"
#include <map>
#include <mutex>
#include <vector>
#include <optional>
#include <string>

namespace dinero {
namespace contracts {

/**
 * ContractRegistry - Manages storage and retrieval of escrow contracts
 *
 * Thread-safe singleton for storing active smart contracts.
 * Provides in-memory storage with optional persistence to disk.
 */
class ContractRegistry {
public:
    // Singleton access
    static ContractRegistry& instance();

    /**
     * Store a new contract
     *
     * @param contract Escrow contract to store
     * @return true if stored successfully
     */
    bool storeContract(const EscrowContract& contract);

    /**
     * Retrieve a contract by ID
     *
     * @param contract_id Unique contract identifier
     * @return Contract if found, nullopt otherwise
     */
    std::optional<EscrowContract> getContract(const std::string& contract_id);

    /**
     * Update existing contract
     *
     * @param contract Updated contract
     * @return true if updated successfully
     */
    bool updateContract(const EscrowContract& contract);

    /**
     * Delete a contract
     *
     * @param contract_id Contract to delete
     * @return true if deleted successfully
     */
    bool deleteContract(const std::string& contract_id);

    /**
     * List all contracts, optionally filtered by address
     *
     * @param address Filter by buyer/seller address (empty = all)
     * @return Vector of matching contracts
     */
    std::vector<EscrowContract> listContracts(const std::string& address = "");

    /**
     * Get total number of contracts
     *
     * @return Count of stored contracts
     */
    size_t getContractCount() const;

    /**
     * Check if contract exists
     *
     * @param contract_id Contract identifier
     * @return true if contract exists
     */
    bool hasContract(const std::string& contract_id) const;

    /**
     * Clear all contracts (for testing)
     */
    void clear();

private:
    ContractRegistry() = default;
    ~ContractRegistry() = default;

    // Prevent copying
    ContractRegistry(const ContractRegistry&) = delete;
    ContractRegistry& operator=(const ContractRegistry&) = delete;

    // In-memory storage
    std::map<std::string, EscrowContract> contracts_;
    mutable std::mutex mutex_;
};

} // namespace contracts
} // namespace dinero
