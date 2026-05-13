#include "contracts/contract_registry.h"
#include "common/logger.h"

namespace dinero {
namespace contracts {

ContractRegistry& ContractRegistry::instance() {
    static ContractRegistry instance;
    return instance;
}

bool ContractRegistry::storeContract(const EscrowContract& contract) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if contract already exists
    if (contracts_.find(contract.contract_id) != contracts_.end()) {
        dinero::g_logger.error("[ContractRegistry] Contract already exists: " +
                              contract.contract_id);
        return false;
    }

    // Store contract
    contracts_[contract.contract_id] = contract;

    dinero::g_logger.info("[ContractRegistry] Stored contract: " + contract.contract_id);
    dinero::g_logger.info("[ContractRegistry]   P2SH: " + contract.p2sh_address);
    dinero::g_logger.info("[ContractRegistry]   Amount: " + std::to_string(contract.amount));

    return true;
}

std::optional<EscrowContract> ContractRegistry::getContract(const std::string& contract_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = contracts_.find(contract_id);
    if (it == contracts_.end()) {
        dinero::g_logger.error("[ContractRegistry] Contract not found: " + contract_id);
        return std::nullopt;
    }

    return it->second;
}

bool ContractRegistry::updateContract(const EscrowContract& contract) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = contracts_.find(contract.contract_id);
    if (it == contracts_.end()) {
        dinero::g_logger.error("[ContractRegistry] Cannot update non-existent contract: " +
                              contract.contract_id);
        return false;
    }

    // Update contract
    contracts_[contract.contract_id] = contract;

    dinero::g_logger.info("[ContractRegistry] Updated contract: " + contract.contract_id);
    dinero::g_logger.info("[ContractRegistry]   Status: " + contract.status);

    return true;
}

bool ContractRegistry::deleteContract(const std::string& contract_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = contracts_.find(contract_id);
    if (it == contracts_.end()) {
        dinero::g_logger.error("[ContractRegistry] Cannot delete non-existent contract: " +
                             contract_id);
        return false;
    }

    contracts_.erase(it);

    dinero::g_logger.info("[ContractRegistry] Deleted contract: " + contract_id);

    return true;
}

std::vector<EscrowContract> ContractRegistry::listContracts(const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<EscrowContract> result;

    for (const auto& [id, contract] : contracts_) {
        // If address filter specified, check if it matches
        if (!address.empty()) {
            // Check if address matches buyer, seller, or mediator
            bool matches = false;

            // Extract pubkey addresses (simplified check)
            if (contract.keys.buyer_pubkey.find(address) != std::string::npos ||
                contract.keys.seller_pubkey.find(address) != std::string::npos ||
                contract.keys.mediator_pubkey.find(address) != std::string::npos) {
                matches = true;
            }

            if (!matches) {
                continue;
            }
        }

        result.push_back(contract);
    }

    dinero::g_logger.info("[ContractRegistry] Listed " + std::to_string(result.size()) +
                         " contracts" + (address.empty() ? "" : " for address " + address));

    return result;
}

size_t ContractRegistry::getContractCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return contracts_.size();
}

bool ContractRegistry::hasContract(const std::string& contract_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return contracts_.find(contract_id) != contracts_.end();
}

void ContractRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    contracts_.clear();
    dinero::g_logger.info("[ContractRegistry] Cleared all contracts");
}

} // namespace contracts
} // namespace dinero
