#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <cstring>

// Windows wingdi.h defines ERROR as a macro
#ifdef ERROR
#undef ERROR
#endif

namespace dinero {

// P2P message constants
// Canonical Dinero mainnet network magic. Must match chainparams and iOS Protocol.swift.
const uint32_t MAGIC_BYTES = 0xD1A0C0DE;
const size_t MESSAGE_HEADER_SIZE = 24;
const size_t MAX_MESSAGE_SIZE = 4 * 1024 * 1024; // 4MB max message size (Bitcoin Core limit)
const size_t MAX_BLOCK_SIZE = 4 * 1024 * 1024; // 4MB max block size
const size_t MAX_P2P_TX_PAYLOAD = 1024 * 1024; // 1MB P2P wire buffer (consensus tx limit is 100KB)
const size_t MAX_INV_SIZE = 50000; // Max inventory items per message
const size_t MAX_ADDR_SIZE = 1000; // Max addresses per addr message
const size_t MAX_HEADERS_SIZE = 2000; // Max headers per headers message
const size_t COMMAND_SIZE = 12;

// P2P message commands
namespace MessageCommands {
    const std::string VERSION = "version";
    const std::string VERACK = "verack";
    const std::string PING = "ping";
    const std::string PONG = "pong";
    const std::string INV = "inv";
    const std::string GETDATA = "getdata";
    const std::string BLOCK = "block";
    const std::string TX = "tx";
    const std::string ADDR = "addr";
    const std::string GETADDR = "getaddr";
    const std::string GETBLOCKS = "getblocks";
    const std::string GETHEADERS = "getheaders";
    const std::string HEADERS = "headers";
    const std::string REJECT = "reject";
    const std::string SENDHEADERS = "sendheaders";
}

// Inventory types
enum class InventoryType : uint32_t {
    ERROR = 0,
    MSG_TX = 1,
    MSG_BLOCK = 2,
    MSG_FILTERED_BLOCK = 3,
    MSG_CMPCT_BLOCK = 4
};

// Inventory vector entry
struct InventoryVector {
    InventoryType type;
    std::string hash; // 32-byte hash as hex string
    
    InventoryVector() : type(InventoryType::ERROR) {}
    InventoryVector(InventoryType t, const std::string& h) : type(t), hash(h) {}
};

// Network address structure
struct NetworkAddress {
    uint64_t services;
    std::string ip;
    uint16_t port;
    uint32_t timestamp; // Only used in addr messages
    
    NetworkAddress() : services(0), port(0), timestamp(0) {}
    NetworkAddress(const std::string& address, uint16_t p, uint64_t srv = 1) 
        : services(srv), ip(address), port(p), timestamp(0) {}
};

// P2P message header
struct MessageHeader {
    uint32_t magic;
    char command[COMMAND_SIZE];
    uint32_t length;
    uint32_t checksum;
    
    MessageHeader() : magic(MAGIC_BYTES), length(0), checksum(0) {
        memset(command, 0, COMMAND_SIZE);
    }
};

// Base P2P message class
class P2PMessage {
public:
    P2PMessage(const std::string& cmd) : command(cmd) {}
    virtual ~P2PMessage() = default;
    
    // Serialization
    virtual std::vector<uint8_t> serialize() const = 0;
    virtual bool deserialize(const std::vector<uint8_t>& data) = 0;
    
    // Getters
    std::string getCommand() const { return command; }
    size_t getSize() const { return serialize().size(); }
    
    // Factory method for creating messages from raw data
    static std::shared_ptr<P2PMessage> createFromData(const std::string& command, const std::vector<uint8_t>& payload);
    
    // Utility functions
    static uint32_t calculateChecksum(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> createMessageFrame(const std::string& command, const std::vector<uint8_t>& payload);
    
protected:
    std::string command;
    
    // Serialization helpers
    void writeUint8(std::vector<uint8_t>& data, uint8_t value) const;
    void writeUint16(std::vector<uint8_t>& data, uint16_t value) const;
    void writeUint32(std::vector<uint8_t>& data, uint32_t value) const;
    void writeUint64(std::vector<uint8_t>& data, uint64_t value) const;
    void writeString(std::vector<uint8_t>& data, const std::string& str) const;
    void writeVarInt(std::vector<uint8_t>& data, uint64_t value) const;
    void writeBytes(std::vector<uint8_t>& data, const std::vector<uint8_t>& bytes) const;
    
    uint8_t readUint8(const std::vector<uint8_t>& data, size_t& pos) const;
    uint16_t readUint16(const std::vector<uint8_t>& data, size_t& pos) const;
    uint32_t readUint32(const std::vector<uint8_t>& data, size_t& pos) const;
    uint64_t readUint64(const std::vector<uint8_t>& data, size_t& pos) const;
    std::string readString(const std::vector<uint8_t>& data, size_t& pos, size_t length) const;
    uint64_t readVarInt(const std::vector<uint8_t>& data, size_t& pos) const;
    std::vector<uint8_t> readBytes(const std::vector<uint8_t>& data, size_t& pos, size_t length) const;
};

// Version message
class VersionMessage : public P2PMessage {
public:
    VersionMessage() : P2PMessage(MessageCommands::VERSION) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    uint32_t version = 70001;
    uint64_t services = 1;
    int64_t timestamp = 0;
    NetworkAddress addr_recv;
    NetworkAddress addr_from;
    uint64_t nonce = 0;
    std::string user_agent;
    int32_t start_height = 0;
    bool relay = true;
};

// Verack message
class VerackMessage : public P2PMessage {
public:
    VerackMessage() : P2PMessage(MessageCommands::VERACK) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
};

// Ping message
class PingMessage : public P2PMessage {
public:
    PingMessage() : P2PMessage(MessageCommands::PING) {}
    PingMessage(uint64_t n) : P2PMessage(MessageCommands::PING), nonce(n) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    uint64_t nonce = 0;
};

// Pong message
class PongMessage : public P2PMessage {
public:
    PongMessage() : P2PMessage(MessageCommands::PONG) {}
    PongMessage(uint64_t n) : P2PMessage(MessageCommands::PONG), nonce(n) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    uint64_t nonce = 0;
};

// Inventory message
class InvMessage : public P2PMessage {
public:
    InvMessage() : P2PMessage(MessageCommands::INV) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    std::vector<InventoryVector> inventory;
};

// Getdata message
class GetdataMessage : public P2PMessage {
public:
    GetdataMessage() : P2PMessage(MessageCommands::GETDATA) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    std::vector<InventoryVector> inventory;
};

// Block message
class BlockMessage : public P2PMessage {
public:
    BlockMessage() : P2PMessage(MessageCommands::BLOCK) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    // Block data will be serialized directly
    std::vector<uint8_t> block_data;
};

// Transaction message
class TxMessage : public P2PMessage {
public:
    TxMessage() : P2PMessage(MessageCommands::TX) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    // Transaction data will be serialized directly
    std::vector<uint8_t> tx_data;
};

// Address message
class AddrMessage : public P2PMessage {
public:
    AddrMessage() : P2PMessage(MessageCommands::ADDR) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    std::vector<NetworkAddress> addresses;
};

// Getaddr message
class GetaddrMessage : public P2PMessage {
public:
    GetaddrMessage() : P2PMessage(MessageCommands::GETADDR) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
};

// Getblocks message
class GetblocksMessage : public P2PMessage {
public:
    GetblocksMessage() : P2PMessage(MessageCommands::GETBLOCKS) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    uint32_t version = 70001;
    std::vector<std::string> block_locator_hashes;
    std::string hash_stop;
};

// Getheaders message
class GetheadersMessage : public P2PMessage {
public:
    GetheadersMessage() : P2PMessage(MessageCommands::GETHEADERS) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    uint32_t version = 70001;
    std::vector<std::string> block_locator_hashes;
    std::string hash_stop;
};

// Headers message
class HeadersMessage : public P2PMessage {
public:
    HeadersMessage() : P2PMessage(MessageCommands::HEADERS) {}
    
    std::vector<uint8_t> serialize() const override;
    bool deserialize(const std::vector<uint8_t>& data) override;
    
    std::vector<std::vector<uint8_t>> headers; // Block headers
};

} // namespace dinero
