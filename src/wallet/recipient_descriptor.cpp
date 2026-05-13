#include "wallet/recipient_descriptor.h"
#include "common/address_script_builder.h"
#include <string>

namespace dinero {

RecipientDescriptor DecodePaymentTarget(const std::string& address) {
    RecipientDescriptor desc;
    desc.original_address = address;

    if (address.empty()) {
        return desc;
    }

    // Transparent: din1.../rdin1.../tdin1... → standard address decoding
    std::vector<uint8_t> script;
    std::string error;
    if (BuildScriptPubKeyFromAddress(address, script, error)) {
        desc.type = RecipientDescriptor::Type::Transparent;
        desc.script_pubkey = script;

        if (address.substr(0, 4) == "din1") desc.is_mainnet = true;
        else if (address.substr(0, 5) == "rdin1") desc.is_regtest = true;
        else if (address.substr(0, 5) == "tdin1") desc.is_testnet = true;
    }

    return desc;
}

} // namespace dinero
