#include "wallet/transaction.h"
#include "crypto/sha256.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <iostream>

namespace dinero {

// ===== TransactionSerializer Implementation =====

void TransactionSerializer::WriteUint32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
}

void TransactionSerializer::WriteUint64(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
    out.push_back((value >> 32) & 0xff);
    out.push_back((value >> 40) & 0xff);
    out.push_back((value >> 48) & 0xff);
    out.push_back((value >> 56) & 0xff);
}

void TransactionSerializer::WriteVarint(std::vector<uint8_t>& out, uint64_t value) {
    if (value < 0xfd) {
        out.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        out.push_back(0xfd);
        out.push_back(value & 0xff);
        out.push_back((value >> 8) & 0xff);
    } else if (value <= 0xffffffff) {
        out.push_back(0xfe);
        WriteUint32(out, static_cast<uint32_t>(value));
    } else {
        out.push_back(0xff);
        WriteUint64(out, value);
    }
}

void TransactionSerializer::WriteBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    WriteVarint(out, data.size());
    out.insert(out.end(), data.begin(), data.end());
}

void TransactionSerializer::WriteString(std::vector<uint8_t>& out, const std::string& str) {
    std::vector<uint8_t> bytes(str.begin(), str.end());
    WriteBytes(out, bytes);
}

std::string TransactionSerializer::DoubleSHA256(const std::vector<uint8_t>& data) {
    auto hash = DoubleSHA256Bytes(data);
    // For display: reverse bytes (Bitcoin convention shows hashes in big-endian)
    std::reverse(hash.begin(), hash.end());
    return ToHex(hash);
}

std::vector<uint8_t> TransactionSerializer::DoubleSHA256Bytes(const std::vector<uint8_t>& data) {
    // First SHA256
    uint8_t hash1[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash1);

    // Second SHA256
    uint8_t hash2[32];
    crypto::CSHA256().Write(hash1, 32).Finalize(hash2);

    // Return raw SHA256 output - NO reversal
    // Consensus rule: store hash bytes as-is (little-endian uint256 identity)
    // Display reversal happens in uint256::ToString() (Bitcoin convention)
    return std::vector<uint8_t>(hash2, hash2 + 32);
}

std::string TransactionSerializer::ToHex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : data) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::vector<uint8_t> TransactionSerializer::FromHex(const std::string& hex) {
    std::vector<uint8_t> result;
    if (hex.size() % 2 != 0) return result;

    for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        result.push_back(byte);
    }
    return result;
}

// ===== Deserialization Helper Functions (Phase I) =====

static uint32_t ReadUint32(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("ReadUint32: insufficient data");
    }
    uint32_t value = data[offset] |
                     (data[offset + 1] << 8) |
                     (data[offset + 2] << 16) |
                     (data[offset + 3] << 24);
    offset += 4;
    return value;
}

static uint64_t ReadUint64(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 8 > data.size()) {
        throw std::runtime_error("ReadUint64: insufficient data");
    }
    uint64_t value = static_cast<uint64_t>(data[offset]) |
                     (static_cast<uint64_t>(data[offset + 1]) << 8) |
                     (static_cast<uint64_t>(data[offset + 2]) << 16) |
                     (static_cast<uint64_t>(data[offset + 3]) << 24) |
                     (static_cast<uint64_t>(data[offset + 4]) << 32) |
                     (static_cast<uint64_t>(data[offset + 5]) << 40) |
                     (static_cast<uint64_t>(data[offset + 6]) << 48) |
                     (static_cast<uint64_t>(data[offset + 7]) << 56);
    offset += 8;
    return value;
}

static uint64_t ReadVarint(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("ReadVarint: insufficient data");
    }

    uint8_t first = data[offset++];
    if (first < 0xfd) {
        return first;
    } else if (first == 0xfd) {
        if (offset + 2 > data.size()) {
            throw std::runtime_error("ReadVarint: insufficient data for 0xfd");
        }
        uint64_t value = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        return value;
    } else if (first == 0xfe) {
        return ReadUint32(data, offset);
    } else { // 0xff
        return ReadUint64(data, offset);
    }
}

static std::vector<uint8_t> ReadBytes(const std::vector<uint8_t>& data, size_t& offset) {
    uint64_t size = ReadVarint(data, offset);
    if (offset + size > data.size()) {
        throw std::runtime_error("ReadBytes: insufficient data");
    }

    std::vector<uint8_t> result(data.begin() + offset, data.begin() + offset + size);
    offset += size;
    return result;
}

// Phase M.0: Read txid as uint256 (binary identity, raw bytes)
static uint256 ReadTxid(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 32 > data.size()) {
        throw std::runtime_error("ReadTxid: insufficient data");
    }

    uint256 result;
    // Read 32 bytes directly (no reversal - uint256 stores in wire format)
    std::memcpy(result.data, &data[offset], 32);
    offset += 32;

    return result;
}

// ===== Transaction Implementation =====

std::vector<uint8_t> Transaction::Serialize(bool include_witness) const {
    std::vector<uint8_t> result;

    // DEBUG: Log vin size at start of serialization
    std::cerr << "🔍 Transaction::Serialize() - vin.size() = " << vin.size() << ", vout.size() = " << vout.size() << ", include_witness=" << include_witness << std::endl;

    // Version (4 bytes, little-endian)
    TransactionSerializer::WriteUint32(result, static_cast<uint32_t>(version));

    // SegWit marker and flag (if witness data present)
    // Support all witness versions (v0, v1/Taproot, v2+)
    if (include_witness && HasWitness()) {
        result.push_back(0x00);  // Marker
        result.push_back(0x01);  // Flag
    }

    // Input count
    TransactionSerializer::WriteVarint(result, vin.size());
    
    // Inputs
    for (const auto& input : vin) {
        // Previous output (txid + vout)
        // Phase M.0: Write uint256 raw bytes directly (no hex conversion, no reversal)
        // uint256 is already in little-endian wire format
        result.insert(result.end(), input.prevout.txid.begin(), input.prevout.txid.end());
        TransactionSerializer::WriteUint32(result, input.prevout.vout);
        
        // ScriptSig (empty for SegWit)
        TransactionSerializer::WriteBytes(result, input.scriptSig);
        
        // Sequence
        TransactionSerializer::WriteUint32(result, input.sequence);
    }
    
    // Output count
    TransactionSerializer::WriteVarint(result, vout.size());
    
    // Outputs
    for (const auto& output : vout) {
        // Check if this is a confidential output
        if (output.is_confidential) {
            // Confidential output format (Phase I):
            // - Value (8 bytes) = 0 (confidential marker)
            // - ScriptPubKey (variable length)
            // - Commitment (33 bytes)
            // - Range proof (variable length)
            // - Nonce (65 bytes for ECDH)

            TransactionSerializer::WriteUint64(result, 0);  // Confidential marker
            TransactionSerializer::WriteBytes(result, output.scriptPubKey);
            TransactionSerializer::WriteBytes(result, output.commitment);
            TransactionSerializer::WriteBytes(result, output.range_proof);
            TransactionSerializer::WriteBytes(result, output.nonce);
        } else {
            // Transparent output format:
            // - Value (8 bytes, little-endian)
            // - ScriptPubKey (variable length)

            TransactionSerializer::WriteUint64(result, output.value);
            TransactionSerializer::WriteBytes(result, output.scriptPubKey);
        }
    }

    // Explicit fee for confidential transactions (Phase G.2 / Phase I)
    // ✅ CRITICAL FIX: Only serialize explicit fee for confidential transactions
    // Transparent transactions (including coinbase) use pure Bitcoin format
    // This ensures mining blocks are Bitcoin-compatible
    if (HasConfidentialOutputs()) {
        // Serialized after outputs, before witness data
        // Format: 1-byte flag + 8-byte fee (if flag = 1)
        if (has_explicit_fee) {
            result.push_back(0x01);  // Explicit fee marker
            TransactionSerializer::WriteUint64(result, explicit_fee);
        } else {
            result.push_back(0x00);  // No explicit fee
        }
    }
    // For transparent transactions, no explicit fee field (Bitcoin-compatible)
    
    // Witness data (if witness present and including witness)
    // Compatible with all witness versions: v0 (SegWit), v1 (Taproot), v2+
    if (include_witness && HasWitness()) {
        for (const auto& input : vin) {
            // Witness stack count
            TransactionSerializer::WriteVarint(result, input.witness.size());

            // Each witness element
            for (const auto& witness_element : input.witness) {
                TransactionSerializer::WriteBytes(result, witness_element);
            }
        }
    }
    
    // Locktime (4 bytes, little-endian)
    TransactionSerializer::WriteUint32(result, lockTime);

    // DEBUG: Log serialized transaction details
    std::string hex_preview;
    for (size_t i = 0; i < std::min(result.size(), size_t(16)); ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", result[i]);
        hex_preview += buf;
    }
    std::cerr << "🔍 Transaction serialized: size=" << result.size() << " bytes, first16=" << hex_preview << std::endl;

    return result;
}

// NOTE: GetBaseSize(), GetSize(), GetWeight(), SerializeHex() moved to src/primitives/transaction.cpp
// (Phase M.0: consensus library access - dinero_consensus doesn't link against dinero_wallet)

uint256 Transaction::GetTxid() const {
    // Txid is hash of non-witness serialization - Phase M.0: returns uint256
    auto bytes = Serialize(false);
    auto hash_bytes = TransactionSerializer::DoubleSHA256Bytes(bytes);
    uint256 result;
    // Phase M.0: Direct memcpy of raw SHA256 output (little-endian internal identity)
    // DoubleSHA256Bytes() now returns raw bytes; reversal only at presentation boundaries
    std::memcpy(result.data, hash_bytes.data(), 32);
    return result;
}

uint256 Transaction::GetWtxid() const {
    // Wtxid is hash of full serialization (with witness) - Phase M.0: returns uint256
    auto bytes = Serialize(true);
    auto hash_bytes = TransactionSerializer::DoubleSHA256Bytes(bytes);
    uint256 result;
    // Phase M.0: Direct memcpy of raw SHA256 output (little-endian internal identity)
    std::memcpy(result.data, hash_bytes.data(), 32);
    return result;
}

// ===== Transaction Deserialization (Phase I) =====

bool TransactionSerializer::Deserialize(Transaction& tx, const std::vector<uint8_t>& data) {
    try {
        // DEBUG: Log deserialization details
        std::string hex_preview;
        for (size_t i = 0; i < std::min(data.size(), size_t(16)); ++i) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", data[i]);
            hex_preview += buf;
        }
        std::cerr << "🔍 Deserialize: data.size()=" << data.size() << " bytes, first16=" << hex_preview << std::endl;

        size_t offset = 0;

        // Version (4 bytes)
        tx.version = static_cast<int32_t>(ReadUint32(data, offset));

        // Check for SegWit marker (0x00 0x01)
        bool has_witness = false;
        if (offset + 2 <= data.size() && data[offset] == 0x00 && data[offset + 1] == 0x01) {
            has_witness = true;
            offset += 2;  // Skip marker and flag
        }

        // Input count
        uint64_t input_count = ReadVarint(data, offset);

        // Inputs
        tx.vin.clear();
        for (uint64_t i = 0; i < input_count; ++i) {
            TxInput input;

            // Previous output txid (32 bytes)
            // Phase M.0: ReadTxid returns uint256 directly (binary identity)
            input.prevout.txid = ReadTxid(data, offset);

            // Previous output vout (4 bytes)
            input.prevout.vout = ReadUint32(data, offset);

            // ScriptSig
            input.scriptSig = ReadBytes(data, offset);

            // Sequence (4 bytes)
            input.sequence = ReadUint32(data, offset);

            tx.vin.push_back(input);
        }

        // Output count
        uint64_t output_count = ReadVarint(data, offset);

        // Outputs
        tx.vout.clear();
        for (uint64_t i = 0; i < output_count; ++i) {
            TxOutput output;

            // Value (8 bytes)
            uint64_t value = ReadUint64(data, offset);

            // ScriptPubKey
            output.scriptPubKey = ReadBytes(data, offset);

            // Check if this is a confidential output (value == 0 indicates confidential)
            if (value == 0 && offset < data.size()) {
                // Peek ahead to see if we have confidential data
                // Confidential outputs have: commitment, range_proof, nonce
                size_t peek_offset = offset;
                try {
                    // Try to read commitment (should be 33 bytes for compressed EC point)
                    std::vector<uint8_t> test_commitment = ReadBytes(data, peek_offset);
                    if (test_commitment.size() == 33) {
                        // This looks like a confidential output
                        output.is_confidential = true;
                        output.value = 0;

                        // Read commitment
                        output.commitment = ReadBytes(data, offset);

                        // Read range proof
                        output.range_proof = ReadBytes(data, offset);

                        // Read nonce
                        output.nonce = ReadBytes(data, offset);
                    } else {
                        // Not confidential, just a regular zero-value output
                        output.is_confidential = false;
                        output.value = 0;
                    }
                } catch (...) {
                    // If parsing fails, treat as regular zero-value output
                    output.is_confidential = false;
                    output.value = 0;
                }
            } else {
                // Transparent output
                output.is_confidential = false;
                output.value = value;
            }

            tx.vout.push_back(output);
        }

        // Explicit fee marker (Phase I)
        // ✅ CRITICAL: Only deserialize explicit fee for confidential transactions
        // This must match the serialization logic (lines 239-248 in Serialize)
        bool has_confidential_outputs = false;
        for (const auto& output : tx.vout) {
            if (output.is_confidential) {
                has_confidential_outputs = true;
                break;
            }
        }

        if (has_confidential_outputs) {
            // 1 byte: 0x00 = no explicit fee, 0x01 = has explicit fee
            if (offset < data.size()) {
                uint8_t explicit_fee_marker = data[offset++];
                if (explicit_fee_marker == 0x01) {
                    tx.has_explicit_fee = true;
                    tx.explicit_fee = ReadUint64(data, offset);
                } else {
                    tx.has_explicit_fee = false;
                    tx.explicit_fee = 0;
                }
            } else {
                tx.has_explicit_fee = false;
                tx.explicit_fee = 0;
            }
        } else {
            // Transparent transaction - no explicit fee field
            tx.has_explicit_fee = false;
            tx.explicit_fee = 0;
        }

        // Witness data (if present)
        if (has_witness) {
            for (size_t i = 0; i < tx.vin.size(); ++i) {
                // Witness stack count
                uint64_t witness_count = ReadVarint(data, offset);

                // Each witness element
                for (uint64_t j = 0; j < witness_count; ++j) {
                    std::vector<uint8_t> witness_element = ReadBytes(data, offset);
                    tx.vin[i].witness.push_back(witness_element);
                }
            }

            // Detect witness version
            tx.DetectWitnessVersion();
        } else {
            tx.witness_version = 0xFF;  // Legacy (no witness)
        }

        // Locktime (4 bytes)
        tx.lockTime = ReadUint32(data, offset);

        std::cerr << "🔍 Deserialize SUCCESS: consumed " << offset << " bytes, vin.size()=" << tx.vin.size() << ", vout.size()=" << tx.vout.size() << std::endl;
        return true;

    } catch (const std::exception& e) {
        // Deserialization failed
        std::cerr << "🔍 Deserialize FAILED: " << e.what() << std::endl;
        return false;
    }
}

// ✅ BULLETPROOF DESERIALIZATION (Bitcoin Core pattern)
// Returns consumed bytes directly - never re-serialize to determine offset!
bool TransactionSerializer::Deserialize(Transaction& tx, const std::vector<uint8_t>& data, size_t& consumed_out) {
    try {
        // DEBUG: Log deserialization details
        std::string hex_preview;
        for (size_t i = 0; i < std::min(data.size(), size_t(16)); ++i) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", data[i]);
            hex_preview += buf;
        }
        std::cerr << "🔍 Deserialize(consumed_out): data.size()=" << data.size() << " bytes, first16=" << hex_preview << std::endl;

        size_t offset = 0;

        // Version (4 bytes)
        tx.version = static_cast<int32_t>(ReadUint32(data, offset));

        // Check for SegWit marker (0x00 0x01)
        bool has_witness = false;
        if (offset + 2 <= data.size() && data[offset] == 0x00 && data[offset + 1] == 0x01) {
            has_witness = true;
            offset += 2;  // Skip marker and flag
        }

        // Input count
        uint64_t input_count = ReadVarint(data, offset);

        // Inputs
        tx.vin.clear();
        for (uint64_t i = 0; i < input_count; ++i) {
            TxInput input;

            // Previous output txid (32 bytes)
            // Phase M.0: ReadTxid returns uint256 directly (binary identity)
            input.prevout.txid = ReadTxid(data, offset);

            // Previous output vout (4 bytes)
            input.prevout.vout = ReadUint32(data, offset);

            // ScriptSig
            input.scriptSig = ReadBytes(data, offset);

            // Sequence (4 bytes)
            input.sequence = ReadUint32(data, offset);

            tx.vin.push_back(input);
        }

        // Output count
        uint64_t output_count = ReadVarint(data, offset);

        // Outputs
        tx.vout.clear();
        for (uint64_t i = 0; i < output_count; ++i) {
            TxOutput output;

            // Value (8 bytes)
            uint64_t value = ReadUint64(data, offset);

            // ScriptPubKey
            output.scriptPubKey = ReadBytes(data, offset);

            // Check if this is a confidential output (value == 0 indicates confidential)
            if (value == 0 && offset < data.size()) {
                // Peek ahead to see if we have confidential data
                size_t peek_offset = offset;
                try {
                    // Try to read commitment (should be 33 bytes for compressed EC point)
                    std::vector<uint8_t> test_commitment = ReadBytes(data, peek_offset);
                    if (test_commitment.size() == 33) {
                        // This looks like a confidential output
                        output.is_confidential = true;
                        output.value = 0;

                        // Read commitment
                        output.commitment = ReadBytes(data, offset);

                        // Read range proof
                        output.range_proof = ReadBytes(data, offset);

                        // Read nonce
                        output.nonce = ReadBytes(data, offset);
                    } else {
                        // Not confidential, just a regular zero-value output
                        output.is_confidential = false;
                        output.value = 0;
                    }
                } catch (...) {
                    // If parsing fails, treat as regular zero-value output
                    output.is_confidential = false;
                    output.value = 0;
                }
            } else {
                // Transparent output
                output.is_confidential = false;
                output.value = value;
            }

            tx.vout.push_back(output);
        }

        // Explicit fee marker (Phase I)
        // ✅ CRITICAL: Only deserialize explicit fee for confidential transactions
        // This must match the serialization logic (lines 239-248 in Serialize)
        bool has_confidential_outputs = false;
        for (const auto& output : tx.vout) {
            if (output.is_confidential) {
                has_confidential_outputs = true;
                break;
            }
        }

        if (has_confidential_outputs) {
            // 1 byte: 0x00 = no explicit fee, 0x01 = has explicit fee
            if (offset < data.size()) {
                uint8_t explicit_fee_marker = data[offset++];
                if (explicit_fee_marker == 0x01) {
                    tx.has_explicit_fee = true;
                    tx.explicit_fee = ReadUint64(data, offset);
                } else {
                    tx.has_explicit_fee = false;
                    tx.explicit_fee = 0;
                }
            } else {
                tx.has_explicit_fee = false;
                tx.explicit_fee = 0;
            }
        } else {
            // Transparent transaction - no explicit fee field
            tx.has_explicit_fee = false;
            tx.explicit_fee = 0;
        }

        // Witness data (if present)
        if (has_witness) {
            for (size_t i = 0; i < tx.vin.size(); ++i) {
                // Witness stack count
                uint64_t witness_count = ReadVarint(data, offset);

                // Each witness element
                for (uint64_t j = 0; j < witness_count; ++j) {
                    std::vector<uint8_t> witness_element = ReadBytes(data, offset);
                    tx.vin[i].witness.push_back(witness_element);
                }
            }

            // Detect witness version
            tx.DetectWitnessVersion();
        } else {
            tx.witness_version = 0xFF;  // Legacy (no witness)
        }

        // Locktime (4 bytes)
        tx.lockTime = ReadUint32(data, offset);

        // ✅ CRITICAL: Return exact consumed bytes (never re-serialize!)
        consumed_out = offset;
        std::cerr << "🔍 Deserialize SUCCESS: consumed " << consumed_out << " bytes, vin.size()=" << tx.vin.size() << ", vout.size()=" << tx.vout.size() << std::endl;
        return true;

    } catch (const std::exception& e) {
        // Deserialization failed
        std::cerr << "🔍 Deserialize FAILED: " << e.what() << std::endl;
        consumed_out = 0;
        return false;
    }
}

bool TransactionSerializer::Deserialize(Transaction& tx, const std::string& hex) {
    std::vector<uint8_t> data = FromHex(hex);
    if (data.empty()) {
        return false;
    }
    return Deserialize(tx, data);
}

} // namespace dinero

