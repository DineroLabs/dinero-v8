#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <map>
#include "wallet/address.h"
#include "common/logger.h"

using namespace dinero;

// Utility function to convert bytes to hex string
std::string bytesToHexStr(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

// Utility function to convert bytes to hex string
std::string bytesToHexStr(const std::array<uint8_t, 32>& bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

// Function to print address metadata in JSON-like format
void printAddressMetadata(const AddressMetadata& metadata) {
    std::cout << "{\n";
    std::cout << "  \"address\": \"" << metadata.address << "\",\n";
    std::cout << "  \"type\": \"";
    
    switch (metadata.type) {
        case AddressType::P2PKH:
            std::cout << "P2PKH";
            break;
        case AddressType::P2SH:
            std::cout << "P2SH";
            break;
        case AddressType::DINERO_P2PKH:
            std::cout << "Dinero P2PKH";
            break;
        case AddressType::DINERO_P2SH:
            std::cout << "Dinero P2SH";
            break;
        default:
            std::cout << "Unknown";
    }
    std::cout << "\",\n";
    
    std::cout << "  \"network\": \"" << metadata.network << "\",\n";
    std::cout << "  \"is_dinero\": " << (metadata.is_dinero ? "true" : "false") << ",\n";
    std::cout << "  \"version_byte\": \"" << metadata.version_byte << "\",\n";
    std::cout << "  \"hex_hash160\": \"" << metadata.hex_hash160 << "\",\n";
    std::cout << "  \"prefix\": \"" << metadata.prefix << "\",\n";
    std::cout << "  \"is_valid\": " << (metadata.is_valid ? "true" : "false");
    
    if (!metadata.error_message.empty()) {
        std::cout << ",\n  \"error_message\": \"" << metadata.error_message << "\"";
    }
    
    std::cout << "\n}\n";
}

void showUsage() {
    std::cout << "🕌 Dinero Advanced Tools\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    std::cout << "Usage: dinero_tools <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  metadata <address>           - Get detailed address metadata\n";
    std::cout << "  qr <address> [size]          - Generate QR code for address\n";
    std::cout << "  vanity <prefix> [type] [max] - Generate vanity address\n";
    std::cout << "  batch <count> [type]         - Generate batch addresses\n";
    std::cout << "  generate [type]              - Generate single address\n";
    std::cout << "  validate <address>           - Validate address\n\n";
    std::cout << "Address Types:\n";
    std::cout << "  p2pkh                        - Bitcoin P2PKH (1...)\n";
    std::cout << "  p2sh                         - Bitcoin P2SH (3...)\n";
    std::cout << "  dinero-p2pkh              - Dinero P2PKH (H...)\n";
    std::cout << "  dinero-p2sh               - Dinero P2SH (7...)\n\n";
    std::cout << "Examples:\n";
    std::cout << "  dinero_tools metadata H8SmqTsJRrgu9tvvYjpiBXk8xehFgpxVnw\n";
    std::cout << "  dinero_tools qr H8SmqTsJRrgu9tvvYjpiBXk8xehFgpxVnw 300\n";
    std::cout << "  dinero_tools vanity HC dinero-p2pkh 5000\n";
    std::cout << "  dinero_tools batch 10 dinero-p2pkh\n";
    std::cout << "  dinero_tools generate dinero-p2pkh\n";
}

AddressType parseAddressType(const std::string& typeStr) {
    if (typeStr == "p2pkh") return AddressType::P2PKH;
    if (typeStr == "p2sh") return AddressType::P2SH;
    if (typeStr == "dinero-p2pkh") return AddressType::DINERO_P2PKH;
    if (typeStr == "dinero-p2sh") return AddressType::DINERO_P2SH;
    return AddressType::DINERO_P2PKH; // default
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        showUsage();
        return 1;
    }
    
    std::string command = argv[1];
    
    if (command == "metadata") {
        if (argc < 3) {
            std::cerr << "❌ Error: Address required for metadata command\n";
            return 1;
        }
        
        std::string address = argv[2];
        std::cout << "🔍 Address Metadata Analysis\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
        
        AddressMetadata metadata = Address::getAddressMetadata(address);
        printAddressMetadata(metadata);
        
    } else if (command == "qr") {
        if (argc < 3) {
            std::cerr << "❌ Error: Address required for QR command\n";
            return 1;
        }
        
        std::string address = argv[2];
        int size = (argc > 3) ? std::stoi(argv[3]) : 200;
        
        std::cout << "🎯 QR Code Generation\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
        
        std::string qrCode = Address::generateQRCode(address, size);
        std::cout << qrCode;
        
    } else if (command == "vanity") {
        if (argc < 3) {
            std::cerr << "❌ Error: Prefix required for vanity command\n";
            return 1;
        }
        
        std::string prefix = argv[2];
        AddressType type = (argc > 3) ? parseAddressType(argv[3]) : AddressType::DINERO_P2PKH;
        int maxAttempts = (argc > 4) ? std::stoi(argv[4]) : 10000;
        
        std::cout << "✨ Vanity Address Generation\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
        std::cout << "🔍 Searching for address with prefix: " << prefix << "\n";
        std::cout << "🎯 Address type: ";
        
        switch (type) {
            case AddressType::DINERO_P2PKH:
                std::cout << "Dinero P2PKH (H...)\n";
                break;
            case AddressType::DINERO_P2SH:
                std::cout << "Dinero P2SH (7...)\n";
                break;
            case AddressType::P2PKH:
                std::cout << "Bitcoin P2PKH (1...)\n";
                break;
            case AddressType::P2SH:
                std::cout << "Bitcoin P2SH (3...)\n";
                break;
        }
        
        std::cout << "⏱️  Max attempts: " << maxAttempts << "\n\n";
        
        std::string vanityAddress = Address::generateVanityAddress(prefix, type, maxAttempts);
        
        if (!vanityAddress.empty()) {
            std::cout << "✅ Success! Found vanity address:\n";
            std::cout << "   " << vanityAddress << "\n\n";
            
            // Show metadata for the generated address
            AddressMetadata metadata = Address::getAddressMetadata(vanityAddress);
            std::cout << "📊 Address Metadata:\n";
            printAddressMetadata(metadata);
        } else {
            std::cout << "❌ Failed to generate vanity address with prefix '" << prefix << "'\n";
            std::cout << "💡 Try increasing max attempts or using a shorter prefix\n";
        }
        
    } else if (command == "batch") {
        if (argc < 3) {
            std::cerr << "❌ Error: Count required for batch command\n";
            return 1;
        }
        
        int count = std::stoi(argv[2]);
        AddressType type = (argc > 3) ? parseAddressType(argv[3]) : AddressType::DINERO_P2PKH;
        
        std::cout << "📦 Batch Address Generation\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
        std::cout << "🎯 Generating " << count << " addresses...\n";
        std::cout << "📋 Address type: ";
        
        switch (type) {
            case AddressType::DINERO_P2PKH:
                std::cout << "Dinero P2PKH (H...)\n";
                break;
            case AddressType::DINERO_P2SH:
                std::cout << "Dinero P2SH (7...)\n";
                break;
            case AddressType::P2PKH:
                std::cout << "Bitcoin P2PKH (1...)\n";
                break;
            case AddressType::P2SH:
                std::cout << "Bitcoin P2SH (3...)\n";
                break;
        }
        std::cout << "\n";
        
        std::vector<std::string> addresses = Address::generateBatchAddresses(count, type);
        
        std::cout << "✅ Successfully generated " << addresses.size() << " addresses:\n\n";
        for (size_t i = 0; i < addresses.size(); i++) {
            std::cout << "  " << (i + 1) << ". " << addresses[i] << "\n";
        }
        
    } else if (command == "generate") {
        AddressType type = (argc > 2) ? parseAddressType(argv[2]) : AddressType::DINERO_P2PKH;
        
        std::cout << "🎲 Single Address Generation\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
        
        // Generate private key
        std::array<uint8_t, 32> privateKey = Address::generatePrivateKey();
        
        // Derive public key
        std::vector<uint8_t> publicKey = Address::derivePublicKey(privateKey, true);
        
        // Create address
        std::string address = Address::createAddress(publicKey, type);
        
        std::cout << "🔑 Private Key: " << bytesToHexStr(privateKey) << "\n";
        std::cout << "🔓 Public Key:  " << bytesToHexStr(publicKey) << "\n";
        std::cout << "📍 Address:     " << address << "\n\n";
        
        // Show metadata
        AddressMetadata metadata = Address::getAddressMetadata(address);
        std::cout << "📊 Address Metadata:\n";
        printAddressMetadata(metadata);
        
    } else if (command == "validate") {
        if (argc < 3) {
            std::cerr << "❌ Error: Address required for validate command\n";
            return 1;
        }
        
        std::string address = argv[2];
        
        std::cout << "🔍 Address Validation\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
        
        bool isValid = Address::validateAddress(address);
        
        if (isValid) {
            std::cout << "✅ Address is valid!\n\n";
            AddressMetadata metadata = Address::getAddressMetadata(address);
            printAddressMetadata(metadata);
        } else {
            std::cout << "❌ Address is invalid!\n";
        }
        
    } else {
        std::cerr << "❌ Unknown command: " << command << "\n\n";
        showUsage();
        return 1;
    }
    
    return 0;
} 