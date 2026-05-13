#pragma once
#include <string>
#include <cstdint>
#include <vector>

namespace din {

/**
 * @brief Bitcoin-compatible descriptor checksum algorithm
 *
 * Implements the checksum format used in Bitcoin Core descriptors.
 * The checksum is an 8-character base32 string appended to descriptors
 * with a '#' separator (e.g., "wpkh(...)#abcd1234").
 *
 * This provides error detection for descriptor strings and ensures
 * compatibility with Bitcoin Core wallet tools.
 */
class DescriptorChecksum {
public:
    /**
     * @brief Compute checksum for a descriptor string
     *
     * @param descriptor The descriptor string without checksum
     * @return 8-character checksum string
     */
    static std::string Compute(const std::string& descriptor);

    /**
     * @brief Verify descriptor checksum
     *
     * @param descriptor_with_checksum Full descriptor with #checksum
     * @return true if checksum is valid
     */
    static bool Verify(const std::string& descriptor_with_checksum);

    /**
     * @brief Add checksum to descriptor
     *
     * @param descriptor Descriptor without checksum
     * @return Descriptor with #checksum appended
     */
    static std::string AddChecksum(const std::string& descriptor);

    /**
     * @brief Strip checksum from descriptor
     *
     * @param descriptor_with_checksum Descriptor with #checksum
     * @return Descriptor without checksum
     */
    static std::string StripChecksum(const std::string& descriptor_with_checksum);

private:
    // Checksum character set (Bitcoin's custom base32)
    static constexpr const char* CHECKSUM_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

    // Generator polynomial for checksum (BCH code)
    static constexpr uint64_t CHECKSUM_GENERATOR[] = {
        0xf5dee51989ULL, 0xa9fdca3312ULL, 0x1bab10e32dULL,
        0x3706b1677aULL, 0x644d626ffdULL
    };

    // Compute polynomial modulo for checksum
    static uint64_t PolyMod(const std::vector<uint64_t>& values);

    // Expand descriptor string to checksum input
    static std::vector<uint64_t> ExpandDescriptor(const std::string& descriptor);
};

} // namespace din
