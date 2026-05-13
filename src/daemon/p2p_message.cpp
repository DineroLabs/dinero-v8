#include "daemon/p2p_message.h"
#include "common/logger.h"
#include "common/sha256d.h"
#include "network/utreexo_messages.h"
#include <cstring>
#include <chrono>
#include <random>

namespace dinero {

// P2PMessage base class implementation

std::shared_ptr<P2PMessage> P2PMessage::createFromData(const std::string& command, const std::vector<uint8_t>& payload) {
    std::shared_ptr<P2PMessage> message;
    
    if (command == MessageCommands::VERSION) {
        message = std::make_shared<VersionMessage>();
    } else if (command == MessageCommands::VERACK) {
        message = std::make_shared<VerackMessage>();
    } else if (command == MessageCommands::PING) {
        message = std::make_shared<PingMessage>();
    } else if (command == MessageCommands::PONG) {
        message = std::make_shared<PongMessage>();
    } else if (command == MessageCommands::INV) {
        message = std::make_shared<InvMessage>();
    } else if (command == MessageCommands::GETDATA) {
        message = std::make_shared<GetdataMessage>();
    } else if (command == MessageCommands::BLOCK) {
        message = std::make_shared<BlockMessage>();
    } else if (command == MessageCommands::TX) {
        message = std::make_shared<TxMessage>();
    } else if (command == MessageCommands::ADDR) {
        message = std::make_shared<AddrMessage>();
    } else if (command == MessageCommands::GETADDR) {
        message = std::make_shared<GetaddrMessage>();
    } else if (command == MessageCommands::GETBLOCKS) {
        message = std::make_shared<GetblocksMessage>();
    } else if (command == MessageCommands::GETHEADERS) {
        message = std::make_shared<GetheadersMessage>();
    } else if (command == MessageCommands::HEADERS) {
        message = std::make_shared<HeadersMessage>();
    } else if (command == MessageCommands::GETUTREEXOPROOF ||
               command == MessageCommands::GETUTREEXOPROOFS) {
        message = std::make_shared<GetUtreexoProofP2PMessage>();
    } else if (command == MessageCommands::UTREEXOPROOF ||
               command == MessageCommands::UTREEXOPROOFS ||
               command == "utreexoproof") {
        message = std::make_shared<UtreexoProofP2PMessage>();
    } else if (command == MessageCommands::GETUTREEXOHDRS) {
        message = std::make_shared<GetUtreexoHeadersP2PMessage>();
    } else if (command == MessageCommands::UTREEXOHDRS ||
               command == MessageCommands::UTREEXOHDRS_ALT) {
        message = std::make_shared<UtreexoHeadersP2PMessage>();
    } else {
        g_logger.warning("Unknown message command: " + command);
        return nullptr;
    }
    
    if (message && !message->deserialize(payload)) {
        g_logger.error("Failed to deserialize message: " + command);
        return nullptr;
    }
    
    return message;
}

uint32_t P2PMessage::calculateChecksum(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return 0;
    }
    
    std::string data_str(data.begin(), data.end());
    std::string hash = Dinero::Common::double_sha256(data_str);
    
    if (hash.length() < 8) {
        return 0;
    }
    
    // Take first 4 bytes of double sha256
    uint32_t checksum = 0;
    for (int i = 0; i < 4; i++) {
        char hex_byte[3] = {hash[i*2], hash[i*2+1], '\0'};
        checksum |= (std::stoul(hex_byte, nullptr, 16) << (i * 8));
    }
    
    return checksum;
}

std::vector<uint8_t> P2PMessage::createMessageFrame(const std::string& command, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    
    // Magic bytes (4 bytes)
    frame.push_back((MAGIC_BYTES >> 0) & 0xFF);
    frame.push_back((MAGIC_BYTES >> 8) & 0xFF);
    frame.push_back((MAGIC_BYTES >> 16) & 0xFF);
    frame.push_back((MAGIC_BYTES >> 24) & 0xFF);
    
    // Command (12 bytes, null-padded)
    std::string cmd = command;
    cmd.resize(COMMAND_SIZE, '\0');
    frame.insert(frame.end(), cmd.begin(), cmd.end());
    
    // Payload length (4 bytes)
    uint32_t length = payload.size();
    frame.push_back((length >> 0) & 0xFF);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back((length >> 16) & 0xFF);
    frame.push_back((length >> 24) & 0xFF);
    
    // Checksum (4 bytes)
    uint32_t checksum = calculateChecksum(payload);
    frame.push_back((checksum >> 0) & 0xFF);
    frame.push_back((checksum >> 8) & 0xFF);
    frame.push_back((checksum >> 16) & 0xFF);
    frame.push_back((checksum >> 24) & 0xFF);
    
    // Payload
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    return frame;
}

// Serialization helper methods
void P2PMessage::writeUint8(std::vector<uint8_t>& data, uint8_t value) const {
    data.push_back(value);
}

void P2PMessage::writeUint16(std::vector<uint8_t>& data, uint16_t value) const {
    data.push_back((value >> 0) & 0xFF);
    data.push_back((value >> 8) & 0xFF);
}

void P2PMessage::writeUint32(std::vector<uint8_t>& data, uint32_t value) const {
    data.push_back((value >> 0) & 0xFF);
    data.push_back((value >> 8) & 0xFF);
    data.push_back((value >> 16) & 0xFF);
    data.push_back((value >> 24) & 0xFF);
}

void P2PMessage::writeUint64(std::vector<uint8_t>& data, uint64_t value) const {
    data.push_back((value >> 0) & 0xFF);
    data.push_back((value >> 8) & 0xFF);
    data.push_back((value >> 16) & 0xFF);
    data.push_back((value >> 24) & 0xFF);
    data.push_back((value >> 32) & 0xFF);
    data.push_back((value >> 40) & 0xFF);
    data.push_back((value >> 48) & 0xFF);
    data.push_back((value >> 56) & 0xFF);
}

void P2PMessage::writeString(std::vector<uint8_t>& data, const std::string& str) const {
    writeVarInt(data, str.length());
    data.insert(data.end(), str.begin(), str.end());
}

void P2PMessage::writeVarInt(std::vector<uint8_t>& data, uint64_t value) const {
    if (value < 0xfd) {
        writeUint8(data, static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        writeUint8(data, 0xfd);
        writeUint16(data, static_cast<uint16_t>(value));
    } else if (value <= 0xffffffff) {
        writeUint8(data, 0xfe);
        writeUint32(data, static_cast<uint32_t>(value));
    } else {
        writeUint8(data, 0xff);
        writeUint64(data, value);
    }
}

void P2PMessage::writeBytes(std::vector<uint8_t>& data, const std::vector<uint8_t>& bytes) const {
    data.insert(data.end(), bytes.begin(), bytes.end());
}

// Deserialization helper methods
uint8_t P2PMessage::readUint8(const std::vector<uint8_t>& data, size_t& pos) const {
    if (pos >= data.size()) return 0;
    return data[pos++];
}

uint16_t P2PMessage::readUint16(const std::vector<uint8_t>& data, size_t& pos) const {
    if (pos + 1 >= data.size()) return 0;
    uint16_t value = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    return value;
}

uint32_t P2PMessage::readUint32(const std::vector<uint8_t>& data, size_t& pos) const {
    if (pos + 3 >= data.size()) return 0;
    uint32_t value = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24);
    pos += 4;
    return value;
}

uint64_t P2PMessage::readUint64(const std::vector<uint8_t>& data, size_t& pos) const {
    if (pos + 7 >= data.size()) return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= (static_cast<uint64_t>(data[pos + i]) << (i * 8));
    }
    pos += 8;
    return value;
}

std::string P2PMessage::readString(const std::vector<uint8_t>& data, size_t& pos, size_t length) const {
    if (pos + length > data.size()) return "";
    std::string str(data.begin() + pos, data.begin() + pos + length);
    pos += length;
    return str;
}

uint64_t P2PMessage::readVarInt(const std::vector<uint8_t>& data, size_t& pos) const {
    if (pos >= data.size()) return 0;
    
    uint8_t first = readUint8(data, pos);
    if (first < 0xfd) {
        return first;
    } else if (first == 0xfd) {
        return readUint16(data, pos);
    } else if (first == 0xfe) {
        return readUint32(data, pos);
    } else {
        return readUint64(data, pos);
    }
}

std::vector<uint8_t> P2PMessage::readBytes(const std::vector<uint8_t>& data, size_t& pos, size_t length) const {
    if (pos + length > data.size()) return {};
    std::vector<uint8_t> bytes(data.begin() + pos, data.begin() + pos + length);
    pos += length;
    return bytes;
}

// VersionMessage implementation
std::vector<uint8_t> VersionMessage::serialize() const {
    std::vector<uint8_t> data;
    
    writeUint32(data, version);
    writeUint64(data, services);
    writeUint64(data, timestamp);
    
    // addr_recv
    writeUint64(data, addr_recv.services);
    // IPv6 mapped IPv4 address (16 bytes)
    for (int i = 0; i < 10; i++) data.push_back(0x00);
    data.push_back(0xff); data.push_back(0xff);
    // IPv4 address (4 bytes) - simplified
    data.push_back(127); data.push_back(0); data.push_back(0); data.push_back(1);
    writeUint16(data, addr_recv.port);
    
    // addr_from
    writeUint64(data, addr_from.services);
    // IPv6 mapped IPv4 address (16 bytes)
    for (int i = 0; i < 10; i++) data.push_back(0x00);
    data.push_back(0xff); data.push_back(0xff);
    // IPv4 address (4 bytes) - simplified
    data.push_back(127); data.push_back(0); data.push_back(0); data.push_back(1);
    writeUint16(data, addr_from.port);
    
    writeUint64(data, nonce);
    writeString(data, user_agent);
    writeUint32(data, start_height);
    writeUint8(data, relay ? 1 : 0);
    
    return data;
}

bool VersionMessage::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    
    if (data.size() < 85) return false; // Minimum version message size
    
    version = readUint32(data, pos);
    services = readUint64(data, pos);
    timestamp = readUint64(data, pos);
    
    // addr_recv
    addr_recv.services = readUint64(data, pos);
    pos += 16; // Skip IPv6 address
    addr_recv.port = readUint16(data, pos);
    
    // addr_from
    addr_from.services = readUint64(data, pos);
    pos += 16; // Skip IPv6 address
    addr_from.port = readUint16(data, pos);
    
    nonce = readUint64(data, pos);
    
    uint64_t user_agent_len = readVarInt(data, pos);
    if (user_agent_len > 0 && pos + user_agent_len <= data.size()) {
        user_agent = readString(data, pos, user_agent_len);
    }
    
    if (pos + 4 <= data.size()) {
        start_height = readUint32(data, pos);
    }
    
    if (pos < data.size()) {
        relay = readUint8(data, pos) != 0;
    }
    
    return true;
}

// VerackMessage implementation
std::vector<uint8_t> VerackMessage::serialize() const {
    return std::vector<uint8_t>(); // Empty payload
}

bool VerackMessage::deserialize(const std::vector<uint8_t>& data) {
    return data.empty(); // Should be empty
}

// PingMessage implementation
std::vector<uint8_t> PingMessage::serialize() const {
    std::vector<uint8_t> data;
    writeUint64(data, nonce);
    return data;
}

bool PingMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() != 8) return false;
    size_t pos = 0;
    nonce = readUint64(data, pos);
    return true;
}

// PongMessage implementation
std::vector<uint8_t> PongMessage::serialize() const {
    std::vector<uint8_t> data;
    writeUint64(data, nonce);
    return data;
}

bool PongMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() != 8) return false;
    size_t pos = 0;
    nonce = readUint64(data, pos);
    return true;
}

// InvMessage implementation
std::vector<uint8_t> InvMessage::serialize() const {
    std::vector<uint8_t> data;
    writeVarInt(data, inventory.size());

    for (const auto& inv : inventory) {
        writeUint32(data, static_cast<uint32_t>(inv.type));
        // Phase M.0: Write uint256 directly as bytes
        for (int i = 0; i < 32; i++) {
            data.push_back(inv.hash.data[i]);
        }
    }

    return data;
}

bool InvMessage::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    uint64_t count = readVarInt(data, pos);

    if (count > 50000) return false; // Sanity check

    inventory.clear();
    inventory.reserve(count);

    for (uint64_t i = 0; i < count; i++) {
        if (pos + 36 > data.size()) return false;

        InventoryVector inv;
        inv.type = static_cast<InventoryType>(readUint32(data, pos));

        auto hash_bytes = readBytes(data, pos, sizeof(inv.hash.data));
        std::memcpy(inv.hash.data, hash_bytes.data(), sizeof(inv.hash.data));

        inventory.push_back(inv);
    }
    
    return true;
}

// GetdataMessage implementation
std::vector<uint8_t> GetdataMessage::serialize() const {
    std::vector<uint8_t> data;
    writeVarInt(data, inventory.size());

    for (const auto& inv : inventory) {
        writeUint32(data, static_cast<uint32_t>(inv.type));
        // Phase M.0: Write uint256 directly as bytes
        for (int i = 0; i < 32; i++) {
            data.push_back(inv.hash.data[i]);
        }
    }

    return data;
}

bool GetdataMessage::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    uint64_t count = readVarInt(data, pos);

    if (count > 50000) return false; // Sanity check

    inventory.clear();
    inventory.reserve(count);

    for (uint64_t i = 0; i < count; i++) {
        if (pos + 36 > data.size()) return false;

        InventoryVector inv;
        inv.type = static_cast<InventoryType>(readUint32(data, pos));

        auto hash_bytes = readBytes(data, pos, sizeof(inv.hash.data));
        std::memcpy(inv.hash.data, hash_bytes.data(), sizeof(inv.hash.data));

        inventory.push_back(inv);
    }

    return true;
}

// BlockMessage implementation
std::vector<uint8_t> BlockMessage::serialize() const {
    return block_data;
}

bool BlockMessage::deserialize(const std::vector<uint8_t>& data) {
    block_data = data;
    return !data.empty();
}

// TxMessage implementation
std::vector<uint8_t> TxMessage::serialize() const {
    return tx_data;
}

bool TxMessage::deserialize(const std::vector<uint8_t>& data) {
    tx_data = data;
    return !data.empty();
}

// AddrMessage implementation
std::vector<uint8_t> AddrMessage::serialize() const {
    std::vector<uint8_t> data;
    writeVarInt(data, addresses.size());
    
    for (const auto& addr : addresses) {
        writeUint32(data, addr.timestamp);
        writeUint64(data, addr.services);
        
        // IPv6 mapped IPv4 address (16 bytes) - simplified
        for (int i = 0; i < 10; i++) data.push_back(0x00);
        data.push_back(0xff); data.push_back(0xff);
        // IPv4 address (4 bytes) - simplified parsing
        data.push_back(127); data.push_back(0); data.push_back(0); data.push_back(1);
        
        writeUint16(data, addr.port);
    }
    
    return data;
}

bool AddrMessage::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    uint64_t count = readVarInt(data, pos);
    
    if (count > 1000) return false; // Sanity check
    
    addresses.clear();
    addresses.reserve(count);
    
    for (uint64_t i = 0; i < count; i++) {
        if (pos + 30 > data.size()) return false;
        
        NetworkAddress addr;
        addr.timestamp = readUint32(data, pos);
        addr.services = readUint64(data, pos);
        pos += 16; // Skip IPv6 address for now
        addr.port = readUint16(data, pos);
        addr.ip = "127.0.0.1"; // Simplified for now
        
        addresses.push_back(addr);
    }
    
    return true;
}

// GetaddrMessage implementation
std::vector<uint8_t> GetaddrMessage::serialize() const {
    return std::vector<uint8_t>(); // Empty payload
}

bool GetaddrMessage::deserialize(const std::vector<uint8_t>& data) {
    return data.empty(); // Should be empty
}

// GetblocksMessage implementation
std::vector<uint8_t> GetblocksMessage::serialize() const {
    std::vector<uint8_t> data;
    
    writeUint32(data, version);
    writeVarInt(data, block_locator_hashes.size());
    
    for (const auto& hash : block_locator_hashes) {
        if (hash.length() == 64) {
            for (size_t i = 0; i < hash.length(); i += 2) {
                std::string byte_str = hash.substr(i, 2);
                uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
                data.push_back(byte);
            }
        } else {
            for (int i = 0; i < 32; i++) data.push_back(0);
        }
    }
    
    // Hash stop
    if (hash_stop.length() == 64) {
        for (size_t i = 0; i < hash_stop.length(); i += 2) {
            std::string byte_str = hash_stop.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            data.push_back(byte);
        }
    } else {
        for (int i = 0; i < 32; i++) data.push_back(0);
    }
    
    return data;
}

bool GetblocksMessage::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    
    if (data.size() < 37) return false; // Minimum size
    
    version = readUint32(data, pos);
    uint64_t hash_count = readVarInt(data, pos);
    
    if (hash_count > 500) return false; // Sanity check
    
    block_locator_hashes.clear();
    block_locator_hashes.reserve(hash_count);
    
    for (uint64_t i = 0; i < hash_count; i++) {
        if (pos + 32 > data.size()) return false;
        
        std::string hash_hex;
        for (int j = 0; j < 32; j++) {
            uint8_t byte = readUint8(data, pos);
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", byte);
            hash_hex += hex;
        }
        block_locator_hashes.push_back(hash_hex);
    }
    
    // Read hash_stop
    if (pos + 32 <= data.size()) {
        std::string hash_hex;
        for (int j = 0; j < 32; j++) {
            uint8_t byte = readUint8(data, pos);
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", byte);
            hash_hex += hex;
        }
        hash_stop = hash_hex;
    }
    
    return true;
}

// GetheadersMessage implementation
std::vector<uint8_t> GetheadersMessage::serialize() const {
    std::vector<uint8_t> data;
    
    writeUint32(data, version);
    writeVarInt(data, block_locator_hashes.size());
    
    for (const auto& hash : block_locator_hashes) {
        if (hash.length() == 64) {
            for (size_t i = 0; i < hash.length(); i += 2) {
                std::string byte_str = hash.substr(i, 2);
                uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
                data.push_back(byte);
            }
        } else {
            for (int i = 0; i < 32; i++) data.push_back(0);
        }
    }
    
    // Hash stop
    if (hash_stop.length() == 64) {
        for (size_t i = 0; i < hash_stop.length(); i += 2) {
            std::string byte_str = hash_stop.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            data.push_back(byte);
        }
    } else {
        for (int i = 0; i < 32; i++) data.push_back(0);
    }
    
    return data;
}

bool GetheadersMessage::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    
    if (data.size() < 37) return false; // Minimum size
    
    version = readUint32(data, pos);
    uint64_t hash_count = readVarInt(data, pos);
    
    if (hash_count > 2000) return false; // Sanity check
    
    block_locator_hashes.clear();
    block_locator_hashes.reserve(hash_count);
    
    for (uint64_t i = 0; i < hash_count; i++) {
        if (pos + 32 > data.size()) return false;
        
        std::string hash_hex;
        for (int j = 0; j < 32; j++) {
            uint8_t byte = readUint8(data, pos);
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", byte);
            hash_hex += hex;
        }
        block_locator_hashes.push_back(hash_hex);
    }
    
    // Read hash_stop
    if (pos + 32 <= data.size()) {
        std::string hash_hex;
        for (int j = 0; j < 32; j++) {
            uint8_t byte = readUint8(data, pos);
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", byte);
            hash_hex += hex;
        }
        hash_stop = hash_hex;
    }
    
    return true;
}

// HeadersMessage implementation
std::vector<uint8_t> HeadersMessage::serialize() const {
    std::vector<uint8_t> data;
    writeVarInt(data, headers.size());
    
    for (const auto& header : headers) {
        writeBytes(data, header);
        writeUint8(data, 0); // Transaction count (0 for headers)
    }
    
    return data;
}

bool HeadersMessage::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    uint64_t count = readVarInt(data, pos);
    
    if (count > 2000) return false; // Sanity check
    
    headers.clear();
    headers.reserve(count);
    
    for (uint64_t i = 0; i < count; i++) {
        if (pos + 128 > data.size()) return false; // Block header is 128 bytes (BlockHeader v1)

        std::vector<uint8_t> header = readBytes(data, pos, 128);
        headers.push_back(header);

        // Skip transaction count
        if (pos < data.size()) {
            readVarInt(data, pos);
        }
    }

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 7: Utreexo Proof Serving Protocol P2P Wrappers
// ═════════════════════════════════════════════════════════════════════════════

// GetUtreexoProofP2PMessage
std::vector<uint8_t> GetUtreexoProofP2PMessage::serialize() const {
    if (!message) {
        return {};
    }
    return message->serialize();
}

bool GetUtreexoProofP2PMessage::deserialize(const std::vector<uint8_t>& data) {
    message = std::make_shared<GetUtreexoProofMessage>();
    return message->deserialize(data);
}

// UtreexoProofP2PMessage
std::vector<uint8_t> UtreexoProofP2PMessage::serialize() const {
    if (!message) {
        return {};
    }
    return message->serialize();
}

bool UtreexoProofP2PMessage::deserialize(const std::vector<uint8_t>& data) {
    message = std::make_shared<UtreexoProofMessage>();
    return message->deserialize(data);
}

// GetUtreexoHeadersP2PMessage
std::vector<uint8_t> GetUtreexoHeadersP2PMessage::serialize() const {
    if (!message) {
        return {};
    }
    return message->serialize();
}

bool GetUtreexoHeadersP2PMessage::deserialize(const std::vector<uint8_t>& data) {
    message = std::make_shared<GetUtreexoHeadersMessage>();
    return message->deserialize(data);
}

// UtreexoHeadersP2PMessage
std::vector<uint8_t> UtreexoHeadersP2PMessage::serialize() const {
    if (!message) {
        return {};
    }
    return message->serialize();
}

bool UtreexoHeadersP2PMessage::deserialize(const std::vector<uint8_t>& data) {
    message = std::make_shared<UtreexoHeadersMessage>();
    return message->deserialize(data);
}

} // namespace dinero
