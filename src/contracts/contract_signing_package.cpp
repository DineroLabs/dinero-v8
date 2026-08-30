#include "contracts/contract_signing_package.h"

#include "crypto/sha256.h"

#include <cctype>

namespace dinero::contracts {
namespace {
void appendU64(std::string& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>((value >> shift) & 0xff));
}
void appendString(std::string& out, const std::string& value) {
    appendU64(out, value.size());
    out.append(value);
}
bool exactHex(const std::string& value, size_t bytes) {
    if (value.size() != bytes * 2) return false;
    for (unsigned char ch : value) if (!std::isxdigit(ch)) return false;
    return true;
}
std::string canonicalPayload(const ContractSigningPackage& p) {
    std::string out("dinero.contract.signing-package.v1", 34);
    appendString(out, p.contract_id);
    appendU64(out, p.action == ContractAction::Release ? 0 : 1);
    appendString(out, p.funding_txid);
    appendU64(out, p.funding_vout);
    appendString(out, p.destination_script_hex);
    appendU64(out, p.amount_una);
    appendString(out, p.chain_id);
    appendU64(out, p.expires_at_height);
    appendString(out, p.unsigned_tx_hex);
    appendString(out, p.sighash_hex);
    return out;
}
}  // namespace

std::string ComputeContractSigningPackageId(const ContractSigningPackage& package) {
    return dinero::crypto::double_sha256(canonicalPayload(package));
}

std::vector<std::string> ValidateContractSigningPackage(
    const ContractSigningPackage& p, const ContractSigningExpectation& e,
    const std::unordered_set<std::string>& consumed) {
    std::vector<std::string> errors;
    if (p.package_id.empty() || p.package_id != ComputeContractSigningPackageId(p)) errors.emplace_back("package_id_mismatch");
    if (p.contract_id != e.contract_id) errors.emplace_back("wrong_contract");
    if (p.action != e.action) errors.emplace_back("wrong_action");
    if (p.funding_txid != e.funding_txid || p.funding_vout != e.funding_vout) errors.emplace_back("wrong_funding_outpoint");
    if (p.destination_script_hex != e.destination_script_hex) errors.emplace_back("wrong_destination");
    if (p.amount_una != e.amount_una) errors.emplace_back("wrong_amount");
    if (p.chain_id != e.chain_id) errors.emplace_back("wrong_chain");
    if (p.expires_at_height < e.current_height) errors.emplace_back("expired");
    if (!exactHex(p.funding_txid, 32)) errors.emplace_back("invalid_funding_txid");
    if (p.destination_script_hex.empty() || p.destination_script_hex.size() % 2 != 0) errors.emplace_back("invalid_destination_script");
    if (p.amount_una == 0) errors.emplace_back("invalid_amount");
    if (p.unsigned_tx_hex.empty() || p.unsigned_tx_hex.size() % 2 != 0) errors.emplace_back("invalid_unsigned_transaction");
    if (!exactHex(p.sighash_hex, 32)) errors.emplace_back("invalid_sighash");
    if (consumed.count(p.package_id) != 0) errors.emplace_back("replay");
    return errors;
}
}  // namespace dinero::contracts
