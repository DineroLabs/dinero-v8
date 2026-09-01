#include "contracts/contract_signing_package.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace dinero::contracts;

static ContractSigningPackage basePackage() {
    ContractSigningPackage p;
    p.contract_id = "contract-a";
    p.action = ContractAction::Release;
    p.funding_txid = std::string(64, 'a');
    p.funding_vout = 2;
    p.destination_script_hex = "5120" + std::string(64, 'b');
    p.amount_una = 42'000;
    p.chain_id = "dinero-mainnet-v8";
    p.expires_at_height = 101;
    p.unsigned_tx_hex = "020000000000";
    p.sighash_hex = std::string(64, 'c');
    p.package_id = ComputeContractSigningPackageId(p);
    return p;
}

static ContractSigningExpectation expectation(const ContractSigningPackage& p) {
    return {p.contract_id, p.action, p.funding_txid, p.funding_vout,
            p.destination_script_hex, p.amount_una, p.chain_id, 100};
}

static bool has(const std::vector<std::string>& errors, const char* value) {
    return std::find(errors.begin(), errors.end(), value) != errors.end();
}

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "contract signing package check failed: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    auto p = basePackage();
    const auto e = expectation(p);
    require(ValidateContractSigningPackage(p, e, {}).empty(), "valid package rejected");

    auto wrongChain = p; wrongChain.chain_id = "regtest"; wrongChain.package_id = ComputeContractSigningPackageId(wrongChain);
    require(has(ValidateContractSigningPackage(wrongChain, e, {}), "wrong_chain"), "wrong chain accepted");
    auto wrongContract = p; wrongContract.contract_id = "contract-b"; wrongContract.package_id = ComputeContractSigningPackageId(wrongContract);
    require(has(ValidateContractSigningPackage(wrongContract, e, {}), "wrong_contract"), "wrong contract accepted");
    auto wrongDestination = p; wrongDestination.destination_script_hex = "51"; wrongDestination.package_id = ComputeContractSigningPackageId(wrongDestination);
    require(has(ValidateContractSigningPackage(wrongDestination, e, {}), "wrong_destination"), "wrong destination accepted");
    auto wrongAmount = p; wrongAmount.amount_una++; wrongAmount.package_id = ComputeContractSigningPackageId(wrongAmount);
    require(has(ValidateContractSigningPackage(wrongAmount, e, {}), "wrong_amount"), "wrong amount accepted");
    auto wrongOutpoint = p; wrongOutpoint.funding_vout++; wrongOutpoint.package_id = ComputeContractSigningPackageId(wrongOutpoint);
    require(has(ValidateContractSigningPackage(wrongOutpoint, e, {}), "wrong_funding_outpoint"), "wrong outpoint accepted");
    auto expired = p; expired.expires_at_height = 99; expired.package_id = ComputeContractSigningPackageId(expired);
    require(has(ValidateContractSigningPackage(expired, e, {}), "expired"), "expired package accepted");
    require(has(ValidateContractSigningPackage(p, e, {p.package_id}), "replay"), "replayed package accepted");
    auto tampered = p; tampered.unsigned_tx_hex += "00";
    require(has(ValidateContractSigningPackage(tampered, e, {}), "package_id_mismatch"), "tampered package accepted");
    std::cout << "contract signing package security tests passed\n";
}
