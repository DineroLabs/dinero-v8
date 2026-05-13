#include "wallet/policy_descriptor.h"
#include "crypto/tagged_hash.h"
#include <stdexcept>

namespace dinero {

using crypto::TaggedHashArray;

std::array<uint8_t, 32> PolicyDescriptor::ComputePolicyId() const {
    // policy_id = TaggedHash("dinero/policy/v1", type || version || params)
    std::vector<uint8_t> preimage;
    preimage.reserve(3 + params.size());

    preimage.push_back(static_cast<uint8_t>(template_type));
    preimage.push_back(template_version & 0xff);
    preimage.push_back((template_version >> 8) & 0xff);
    preimage.insert(preimage.end(), params.begin(), params.end());

    return TaggedHashArray("dinero/policy/v1", preimage);
}

std::vector<uint8_t> PolicyDescriptor::Serialize() const {
    std::vector<uint8_t> out;
    // type(1) + version(2) + params_len(2) + params(N)
    out.reserve(5 + params.size());

    out.push_back(static_cast<uint8_t>(template_type));
    out.push_back(template_version & 0xff);
    out.push_back((template_version >> 8) & 0xff);

    uint16_t params_len = static_cast<uint16_t>(params.size());
    out.push_back(params_len & 0xff);
    out.push_back((params_len >> 8) & 0xff);

    out.insert(out.end(), params.begin(), params.end());
    return out;
}

PolicyDescriptor PolicyDescriptor::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 5) {
        throw std::runtime_error("PolicyDescriptor: data too short");
    }

    PolicyDescriptor desc;
    desc.template_type = static_cast<PolicyTemplate>(data[0]);
    desc.template_version = static_cast<uint16_t>(data[1]) |
                            (static_cast<uint16_t>(data[2]) << 8);

    uint16_t params_len = static_cast<uint16_t>(data[3]) |
                          (static_cast<uint16_t>(data[4]) << 8);

    if (data.size() < 5 + params_len) {
        throw std::runtime_error("PolicyDescriptor: data truncated");
    }

    desc.params.assign(data.begin() + 5, data.begin() + 5 + params_len);
    return desc;
}

std::string PolicyDescriptor::TemplateName(PolicyTemplate t) {
    switch (t) {
        case PolicyTemplate::STANDARD:  return "Standard";
        case PolicyTemplate::PROTECTED: return "Protected";
        case PolicyTemplate::ESCROW:    return "Escrow";
        default:                        return "Unknown";
    }
}

} // namespace dinero
