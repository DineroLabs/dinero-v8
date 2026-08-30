#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace dinero::contracts {

enum class ContractAction { Release, Refund };

struct ContractSigningPackage {
    std::string package_id;
    std::string contract_id;
    ContractAction action{ContractAction::Release};
    std::string funding_txid;
    uint32_t funding_vout{0};
    std::string destination_script_hex;
    uint64_t amount_una{0};
    std::string chain_id;
    uint64_t expires_at_height{0};
    std::string unsigned_tx_hex;
    std::string sighash_hex;
};

struct ContractSigningExpectation {
    std::string contract_id;
    ContractAction action{ContractAction::Release};
    std::string funding_txid;
    uint32_t funding_vout{0};
    std::string destination_script_hex;
    uint64_t amount_una{0};
    std::string chain_id;
    uint64_t current_height{0};
};

std::string ComputeContractSigningPackageId(const ContractSigningPackage& package);
std::vector<std::string> ValidateContractSigningPackage(
    const ContractSigningPackage& package,
    const ContractSigningExpectation& expected,
    const std::unordered_set<std::string>& consumed_package_ids);

}  // namespace dinero::contracts
