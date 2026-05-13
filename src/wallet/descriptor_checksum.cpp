// SPDX-License-Identifier: MIT
// Dinero - Bitcoin-compatible Descriptor Checksum Implementation

#include "wallet/descriptor_checksum.h"
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace din {

// Character mapping for input descriptor
static const std::string INPUT_CHARSET =
    "0123456789()[],'/*abcdefgh@:$%{}"
    "IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~"
    "ijklmnopqrstuvwxyzABCDEFGH`#\"\\ ";

uint64_t DescriptorChecksum::PolyMod(const std::vector<uint64_t>& values) {
    uint64_t c = 1;
    for (const auto value : values) {
        uint8_t c0 = c >> 35;
        c = ((c & 0x7ffffffffULL) << 5) ^ value;

        if (c0 & 1)  c ^= CHECKSUM_GENERATOR[0];
        if (c0 & 2)  c ^= CHECKSUM_GENERATOR[1];
        if (c0 & 4)  c ^= CHECKSUM_GENERATOR[2];
        if (c0 & 8)  c ^= CHECKSUM_GENERATOR[3];
        if (c0 & 16) c ^= CHECKSUM_GENERATOR[4];
    }
    return c;
}

std::vector<uint64_t> DescriptorChecksum::ExpandDescriptor(const std::string& descriptor) {
    std::vector<uint64_t> values;
    values.reserve(descriptor.size());

    for (char c : descriptor) {
        size_t pos = INPUT_CHARSET.find(c);
        if (pos == std::string::npos) {
            // Invalid character - use 0 as fallback
            values.push_back(0);
        } else {
            values.push_back(static_cast<uint64_t>(pos));
        }
    }

    return values;
}

std::string DescriptorChecksum::Compute(const std::string& descriptor) {
    // Expand descriptor to polynomial input
    auto values = ExpandDescriptor(descriptor);

    // Append 8 zeros for checksum computation
    values.insert(values.end(), 8, 0);

    // Compute checksum polynomial
    uint64_t checksum = PolyMod(values) ^ 1;

    // Convert to 8-character string
    std::string result(8, ' ');
    for (int i = 0; i < 8; ++i) {
        result[7 - i] = CHECKSUM_CHARSET[(checksum >> (5 * i)) & 31];
    }

    return result;
}

bool DescriptorChecksum::Verify(const std::string& descriptor_with_checksum) {
    // Find checksum separator
    size_t pos = descriptor_with_checksum.rfind('#');
    if (pos == std::string::npos) {
        return false;  // No checksum present
    }

    // Extract descriptor and checksum
    std::string descriptor = descriptor_with_checksum.substr(0, pos);
    std::string provided_checksum = descriptor_with_checksum.substr(pos + 1);

    if (provided_checksum.length() != 8) {
        return false;  // Invalid checksum length
    }

    // Compute expected checksum
    std::string expected_checksum = Compute(descriptor);

    return provided_checksum == expected_checksum;
}

std::string DescriptorChecksum::AddChecksum(const std::string& descriptor) {
    // Strip any existing checksum first
    size_t pos = descriptor.rfind('#');
    std::string clean_descriptor = (pos != std::string::npos)
        ? descriptor.substr(0, pos)
        : descriptor;

    // Compute and append checksum
    std::string checksum = Compute(clean_descriptor);
    return clean_descriptor + "#" + checksum;
}

std::string DescriptorChecksum::StripChecksum(const std::string& descriptor_with_checksum) {
    size_t pos = descriptor_with_checksum.rfind('#');
    if (pos == std::string::npos) {
        return descriptor_with_checksum;  // No checksum to strip
    }
    return descriptor_with_checksum.substr(0, pos);
}

} // namespace din
