// SUBMIT BLOCK 1 TO NODE
// Constructs complete block 1 hex and submits via RPC

#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <cstdint>

#include "consensus/chain_bundle_generated.h"

void write_i32_le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

std::vector<uint8_t> hex_to_bytes(const char* hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < std::strlen(hex); i += 2) {
        uint8_t byte = 0;
        for (int j = 0; j < 2; j++) {
            byte <<= 4;
            char c = hex[i + j];
            if (c >= '0' && c <= '9') byte |= c - '0';
            else if (c >= 'a' && c <= 'f') byte |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') byte |= c - 'A' + 10;
        }
        bytes.push_back(byte);
    }
    return bytes;
}

void reverse_bytes(uint8_t* data, size_t len) {
    for (size_t i = 0; i < len / 2; i++) {
        std::swap(data[i], data[len - 1 - i]);
    }
}

int main() {
    std::cout << "Building block 1 hex...\n\n";

    // Block 1 parameters (from mine_block_1_premine output)
    uint32_t nVersion = 1;
    const char* prevBlockHash = dinero::chain_bundle::GENESIS_BLOCK_HASH;
    const char* merkleRoot = "ce6ed246e734244bd1ffa57e0dd949040b00619013ca6bb4767837076d647855";
    uint32_t nTime = 1764028920;
    uint32_t nBits = dinero::chain_bundle::GENESIS_DIFFICULTY;
    uint32_t nNonce = 12099;

    // Coinbase transaction hex (from miner)
    const char* coinbaseHex =
        "0100000001000000000000000000000000000000000000000000000000000000"
        "0000000000ffffffff4501015072656d696e653a20322c3632372c3930302044"
        "4e5220666f7220646576656c6f706d656e742c206d61726b6574696e672c2065"
        "636f73797374656d2067726f777468ffffffff0100bc999001ef0000225120c2"
        "a63bf0587d7be826218adea70e91759f85b87ca0aa2adaa8e541e601fa0aa000"
        "000000";

    // Build block header (80 bytes)
    std::vector<uint8_t> block;

    // Version (4 bytes, little-endian)
    uint8_t version[4];
    write_i32_le(version, nVersion);
    block.insert(block.end(), version, version + 4);

    // Previous block hash (32 bytes, little-endian)
    auto prevHash = hex_to_bytes(prevBlockHash);
    reverse_bytes(prevHash.data(), 32);
    block.insert(block.end(), prevHash.begin(), prevHash.end());

    // Merkle root (32 bytes, as-is from coinbase TXID)
    auto merkle = hex_to_bytes(merkleRoot);
    block.insert(block.end(), merkle.begin(), merkle.end());

    // Time (4 bytes, little-endian)
    uint8_t time[4];
    write_i32_le(time, nTime);
    block.insert(block.end(), time, time + 4);

    // Bits (4 bytes, little-endian)
    uint8_t bits[4];
    write_i32_le(bits, nBits);
    block.insert(block.end(), bits, bits + 4);

    // Nonce (4 bytes, little-endian)
    uint8_t nonce[4];
    write_i32_le(nonce, nNonce);
    block.insert(block.end(), nonce, nonce + 4);

    // Transaction count (varint: 1 transaction)
    block.push_back(0x01);

    // Coinbase transaction
    auto coinbase = hex_to_bytes(coinbaseHex);
    block.insert(block.end(), coinbase.begin(), coinbase.end());

    // Output complete block hex
    std::cout << "Block 1 hex (" << block.size() << " bytes):\n";
    for (size_t i = 0; i < block.size(); i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)block[i];
    }
    std::cout << std::dec << "\n\n";

    std::cout << "To submit to node, run:\n";
    std::cout << "curl -s --user dinerouser:dineropass \\\n";
    std::cout << "  --data-binary '{\"jsonrpc\":\"1.0\",\"id\":\"submit\",\"method\":\"mining.submitblock\",\"params\":[\"";

    for (size_t i = 0; i < block.size(); i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)block[i];
    }
    std::cout << "\"]}' \\\n";
    std::cout << "  -H 'content-type: text/plain;' http://127.0.0.1:20998/\n";

    return 0;
}
