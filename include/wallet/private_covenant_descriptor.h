#pragma once
#include "wallet/shielded_derivation.h"
#include "consensus/shielded/private_covenant.h"
#include <optional>
#include <openssl/crypto.h>

namespace dinero::wallet {
struct PrivateCovenantPayee {
    shielded::AddressPayload address{};
    uint64_t value_una = 0;
};
struct PrivateCovenantDescriptor {
    uint32_t minimum_height = 0;
    uint64_t fee_una = 0;
    consensus::shielded::Hash seed{};
    std::vector<PrivateCovenantPayee> outputs;
    ~PrivateCovenantDescriptor() { OPENSSL_cleanse(seed.data(), seed.size()); }
};
struct PrivateCovenantOutputMaterial {
    shielded::DecodedShieldedAddress recipient;
    uint64_t value_una = 0;
    consensus::shielded::Hash rcm{}, esk{};
    consensus::shielded::ShieldedOutput output;
    ~PrivateCovenantOutputMaterial() {
        OPENSSL_cleanse(rcm.data(), rcm.size());
        OPENSSL_cleanse(esk.data(), esk.size());
    }
};
bool IsPrivateCovenantMemo(const std::array<uint8_t,512>& memo);
std::array<uint8_t,512> EncodePrivateCovenantDescriptor(const PrivateCovenantDescriptor& descriptor);
std::optional<PrivateCovenantDescriptor> DecodePrivateCovenantDescriptor(const std::array<uint8_t,512>& memo);
uint64_t PrivateCovenantFundingValue(const PrivateCovenantDescriptor& descriptor);
std::vector<PrivateCovenantOutputMaterial> DerivePrivateCovenantOutputs(const PrivateCovenantDescriptor& descriptor);
consensus::shielded::Hash PrivateCovenantDescriptorRoot(const PrivateCovenantDescriptor& descriptor);
}
