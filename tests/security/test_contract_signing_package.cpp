#include "contracts/contract_signing_package.h"

#include <algorithm>
#include <cassert>
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

int main() {
    auto p = basePackage();
    const auto e = expectation(p);
    assert(ValidateContractSigningPackage(p, e, {}).empty());

    auto wrongChain = p; wrongChain.chain_id = "regtest"; wrongChain.package_id = ComputeContractSigningPackageId(wrongChain);
    assert(has(ValidateContractSigningPackage(wrongChain, e, {}), "wrong_chain"));
    auto wrongContract = p; wrongContract.contract_id = "contract-b"; wrongContract.package_id = ComputeContractSigningPackageId(wrongContract);
    assert(has(ValidateContractSigningPackage(wrongContract, e, {}), "wrong_contract"));
    auto wrongDestination = p; wrongDestination.destination_script_hex = "51"; wrongDestination.package_id = ComputeContractSigningPackageId(wrongDestination);
    assert(has(ValidateContractSigningPackage(wrongDestination, e, {}), "wrong_destination"));
    auto wrongAmount = p; wrongAmount.amount_una++; wrongAmount.package_id = ComputeContractSigningPackageId(wrongAmount);
    assert(has(ValidateContractSigningPackage(wrongAmount, e, {}), "wrong_amount"));
    auto wrongOutpoint = p; wrongOutpoint.funding_vout++; wrongOutpoint.package_id = ComputeContractSigningPackageId(wrongOutpoint);
    assert(has(ValidateContractSigningPackage(wrongOutpoint, e, {}), "wrong_funding_outpoint"));
    auto expired = p; expired.expires_at_height = 99; expired.package_id = ComputeContractSigningPackageId(expired);
    assert(has(ValidateContractSigningPackage(expired, e, {}), "expired"));
    assert(has(ValidateContractSigningPackage(p, e, {p.package_id}), "replay"));
    auto tampered = p; tampered.unsigned_tx_hex += "00";
    assert(has(ValidateContractSigningPackage(tampered, e, {}), "package_id_mismatch"));
    std::cout << "contract signing package security tests passed\n";
}
