#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include "wallet/address.h"
#include "common/sha256d.h"
#include "common/logger.h"

using namespace dinero;
using namespace Dinero::Common;

// Utility function to convert bytes to hex string (similar to Bitcoin Core's HexStr)
std::string bytesToHexStr(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

// Utility function to parse hex string to bytes
std::vector<uint8_t> parseHex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        if (i + 1 < hex.length()) {
            std::string byteString = hex.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoi(byteString, nullptr, 16));
            bytes.push_back(byte);
        }
    }
    return bytes;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: validate_address <Base58Check address>" << std::endl;
        std::cerr << "Example: validate_address 1EFN6ZQDF4xLH8wQNtreE5GoCQzaYA8zd4" << std::endl;
        return 1;
    }

    std::string address = argv[1];
    std::vector<uint8_t> decoded;

    std::cout << "🔍 Validating address: " << address << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    // Try to decode the Base58Check address
    if (!Address::decodeBase58Check(address, decoded)) {
        std::cerr << "❌ Invalid Base58Check address: " << address << std::endl;
        std::cerr << "   • Check if the address format is correct" << std::endl;
        std::cerr << "   • Verify the checksum is valid" << std::endl;
        return 1;
    }

    if (decoded.size() < 1) {
        std::cerr << "❌ Address decoded but too short." << std::endl;
        return 1;
    }

    // Extract version and payload
    unsigned char version = decoded[0];
    std::vector<uint8_t> payload(decoded.begin() + 1, decoded.end());

    std::cout << "✅ Address is valid!" << std::endl;
    std::cout << "• Address: " << address << std::endl;
    std::cout << "• Version byte: 0x" << bytesToHexStr({version}) << std::endl;

    // Custom Dinero detection with beautiful output
    if (version == 0x28) {
        std::cout << "• Prefix: H" << std::endl;
        std::cout << "• Type: Dinero P2PKH (Pay-to-PubKey-Hash)" << std::endl;
        std::cout << "• Network: Mainnet" << std::endl;
        std::cout << "• Brand: 🕌 Dinero" << std::endl;
    } else if (version == 0x10) {
        std::cout << "• Prefix: 7" << std::endl;
        std::cout << "• Type: Dinero P2SH (Pay-to-Script-Hash)" << std::endl;
        std::cout << "• Network: Mainnet" << std::endl;
        std::cout << "• Brand: 🕌 Dinero" << std::endl;
    } else if (version == 0x00) {
        std::cout << "• Prefix: 1" << std::endl;
        std::cout << "• Type: Bitcoin P2PKH (Pay-to-PubKey-Hash)" << std::endl;
        std::cout << "• Network: Mainnet" << std::endl;
        std::cout << "• Brand: ₿ Bitcoin" << std::endl;
    } else if (version == 0x05) {
        std::cout << "• Prefix: 3" << std::endl;
        std::cout << "• Type: Bitcoin P2SH (Pay-to-Script-Hash)" << std::endl;
        std::cout << "• Network: Mainnet" << std::endl;
        std::cout << "• Brand: ₿ Bitcoin" << std::endl;
    } else if (version == 0x6F) {
        std::cout << "• Prefix: m/n" << std::endl;
        std::cout << "• Type: Bitcoin P2PKH (Pay-to-PubKey-Hash)" << std::endl;
        std::cout << "• Network: Testnet" << std::endl;
        std::cout << "• Brand: ₿ Bitcoin" << std::endl;
    } else if (version == 0xC4) {
        std::cout << "• Prefix: 2" << std::endl;
        std::cout << "• Type: Bitcoin P2SH (Pay-to-Script-Hash)" << std::endl;
        std::cout << "• Network: Testnet" << std::endl;
        std::cout << "• Brand: ₿ Bitcoin" << std::endl;
    } else {
        std::cout << "• Prefix: Unknown" << std::endl;
        std::cout << "• Type: Unknown address version (custom or future format)" << std::endl;
        std::cout << "• Network: Unknown" << std::endl;
        std::cout << "• Brand: ❓ Unknown" << std::endl;
    }

    std::cout << "• Payload (hash160): " << bytesToHexStr(payload) << std::endl;
    std::cout << "• Total decoded length: " << decoded.size() << " bytes" << std::endl;

    // Additional validation checks
    if (payload.size() == 20) {
        std::cout << "• Hash160 length: " << payload.size() << " bytes (correct)" << std::endl;
    } else {
        std::cout << "• Hash160 length: " << payload.size() << " bytes (unexpected)" << std::endl;
    }

    // Show the full decoded data
    std::cout << "• Full decoded data: " << bytesToHexStr(decoded) << std::endl;

    // Verify the address can be re-encoded correctly
    std::string reencoded = Address::encodeBase58Check(decoded);
    if (reencoded == address) {
        std::cout << "• Re-encoding verification: ✅ PASSED" << std::endl;
    } else {
        std::cout << "• Re-encoding verification: ❌ FAILED" << std::endl;
        std::cout << "  Expected: " << address << std::endl;
        std::cout << "  Got:      " << reencoded << std::endl;
    }

    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "🎯 Address validation completed successfully!" << std::endl;

    return 0;
} 