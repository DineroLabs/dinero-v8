/**
 * Phase 36: Transaction Deserialization Fuzz Harness
 *
 * This fuzzer exercises the transaction parser with random/malformed inputs
 * to find crashes, memory corruption, and undefined behavior.
 *
 * Targets:
 * - Legacy transaction parsing
 * - SegWit transaction parsing (witness marker 0x0001)
 * - Input/output parsing
 * - Script parsing
 * - Witness data parsing
 * - Size calculations
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_tx_deserialize.cpp ../src/wallet/transaction.cpp \
 *           -lssl -lcrypto -o fuzz_tx_deserialize
 *
 * Run:
 *   ./fuzz_tx_deserialize corpus/tx/ -max_len=100000
 */

#include "wallet/transaction.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#include <stdexcept>

using namespace dinero;

// Standalone transaction deserializer for fuzzing
// This is a simplified version that focuses on parsing without validation
class TxDeserializer {
public:
    static bool Deserialize(Transaction& tx, const uint8_t* data, size_t size) {
        if (size < 10) {  // Minimum tx size
            return false;
        }

        size_t pos = 0;

        // Read version (4 bytes)
        if (pos + 4 > size) return false;
        tx.version = static_cast<int32_t>(
            data[pos] | (data[pos + 1] << 8) |
            (data[pos + 2] << 16) | (data[pos + 3] << 24)
        );
        pos += 4;

        // Check for SegWit marker (0x00 0x01)
        bool is_segwit = false;
        if (pos + 2 <= size && data[pos] == 0x00 && data[pos + 1] == 0x01) {
            is_segwit = true;
            tx.witness_version = 0;  // SegWit v0
            pos += 2;
        } else {
            tx.witness_version = 0xFF;  // Legacy
        }

        // Read input count
        uint64_t input_count;
        if (!readVarInt(data, size, pos, input_count)) return false;
        if (input_count > 10000) return false;  // Sanity limit

        // Read inputs
        tx.vin.resize(input_count);
        for (uint64_t i = 0; i < input_count; ++i) {
            if (!readInput(data, size, pos, tx.vin[i])) return false;
        }

        // Read output count
        uint64_t output_count;
        if (!readVarInt(data, size, pos, output_count)) return false;
        if (output_count > 10000) return false;  // Sanity limit

        // Read outputs
        tx.vout.resize(output_count);
        for (uint64_t i = 0; i < output_count; ++i) {
            if (!readOutput(data, size, pos, tx.vout[i])) return false;
        }

        // Read witness data if SegWit
        if (is_segwit) {
            for (uint64_t i = 0; i < input_count; ++i) {
                uint64_t witness_count;
                if (!readVarInt(data, size, pos, witness_count)) return false;
                if (witness_count > 500) return false;  // Sanity limit

                tx.vin[i].witness.resize(witness_count);
                for (uint64_t j = 0; j < witness_count; ++j) {
                    uint64_t witness_len;
                    if (!readVarInt(data, size, pos, witness_len)) return false;
                    if (witness_len > 10000) return false;  // Sanity limit

                    if (pos + witness_len > size) return false;
                    tx.vin[i].witness[j].assign(data + pos, data + pos + witness_len);
                    pos += witness_len;
                }
            }
        }

        // Read locktime (4 bytes)
        if (pos + 4 > size) return false;
        tx.lockTime = static_cast<uint32_t>(
            data[pos] | (data[pos + 1] << 8) |
            (data[pos + 2] << 16) | (data[pos + 3] << 24)
        );
        pos += 4;

        return true;
    }

private:
    static bool readVarInt(const uint8_t* data, size_t size, size_t& pos, uint64_t& value) {
        if (pos >= size) return false;

        uint8_t first = data[pos++];

        if (first < 0xFD) {
            value = first;
            return true;
        } else if (first == 0xFD) {
            if (pos + 2 > size) return false;
            value = static_cast<uint16_t>(data[pos]) |
                    (static_cast<uint16_t>(data[pos + 1]) << 8);
            pos += 2;
            return true;
        } else if (first == 0xFE) {
            if (pos + 4 > size) return false;
            value = static_cast<uint32_t>(data[pos]) |
                    (static_cast<uint32_t>(data[pos + 1]) << 8) |
                    (static_cast<uint32_t>(data[pos + 2]) << 16) |
                    (static_cast<uint32_t>(data[pos + 3]) << 24);
            pos += 4;
            return true;
        } else {
            if (pos + 8 > size) return false;
            value = 0;
            for (int i = 0; i < 8; ++i) {
                value |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
            }
            pos += 8;
            return true;
        }
    }

    static bool readInput(const uint8_t* data, size_t size, size_t& pos, TxInput& input) {
        // Previous output hash (32 bytes)
        if (pos + 32 > size) return false;
        std::memcpy(input.prevout.txid.v.data, data + pos, 32);
        pos += 32;

        // Previous output index (4 bytes)
        if (pos + 4 > size) return false;
        input.prevout.vout = static_cast<uint32_t>(
            data[pos] | (data[pos + 1] << 8) |
            (data[pos + 2] << 16) | (data[pos + 3] << 24)
        );
        pos += 4;

        // ScriptSig length and data
        uint64_t script_len;
        if (!readVarInt(data, size, pos, script_len)) return false;
        if (script_len > 10000) return false;  // Max script size

        if (pos + script_len > size) return false;
        input.scriptSig.assign(data + pos, data + pos + script_len);
        pos += script_len;

        // Sequence (4 bytes)
        if (pos + 4 > size) return false;
        input.sequence = static_cast<uint32_t>(
            data[pos] | (data[pos + 1] << 8) |
            (data[pos + 2] << 16) | (data[pos + 3] << 24)
        );
        pos += 4;

        return true;
    }

    static bool readOutput(const uint8_t* data, size_t size, size_t& pos, TxOutput& output) {
        // Value (8 bytes)
        if (pos + 8 > size) return false;
        uint64_t raw_value = 0;
        for (int i = 0; i < 8; ++i) {
            raw_value |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
        }
        output.value = AmountUna::UnsafeFromRaw(raw_value);
        pos += 8;

        // ScriptPubKey length and data
        uint64_t script_len;
        if (!readVarInt(data, size, pos, script_len)) return false;
        if (script_len > 10000) return false;  // Max script size

        if (pos + script_len > size) return false;
        output.scriptPubKey.assign(data + pos, data + pos + script_len);
        pos += script_len;

        return true;
    }
};

// Fuzz modes
enum TxFuzzMode : uint8_t {
    FUZZ_FULL_DESERIALIZE = 0,    // Full transaction deserialization
    FUZZ_LEGACY_TX = 1,           // Force legacy parsing
    FUZZ_SEGWIT_TX = 2,           // Force SegWit parsing
    FUZZ_TX_SERIALIZE = 3,        // Deserialize then serialize
    FUZZ_TX_METHODS = 4,          // Test Transaction methods
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }

    TxFuzzMode mode = static_cast<TxFuzzMode>(data[0] % 5);
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    switch (mode) {
        case FUZZ_FULL_DESERIALIZE: {
            // Standard deserialization
            Transaction tx;
            bool result = TxDeserializer::Deserialize(tx, payload, payload_size);
            if (result) {
                // Access fields to catch memory issues
                (void)tx.version;
                (void)tx.vin.size();
                (void)tx.vout.size();
                (void)tx.lockTime;
            }
            break;
        }

        case FUZZ_LEGACY_TX: {
            // Ensure input doesn't look like SegWit
            std::vector<uint8_t> modified(payload, payload + payload_size);
            if (modified.size() >= 6) {
                // Clear potential SegWit marker
                if (modified[4] == 0x00 && modified[5] == 0x01) {
                    modified[5] = 0x02;  // Break SegWit marker
                }
            }
            Transaction tx;
            (void)TxDeserializer::Deserialize(tx, modified.data(), modified.size());
            break;
        }

        case FUZZ_SEGWIT_TX: {
            // Ensure input looks like SegWit
            std::vector<uint8_t> modified(payload, payload + payload_size);
            if (modified.size() >= 6) {
                // Add SegWit marker
                modified[4] = 0x00;
                modified[5] = 0x01;
            }
            Transaction tx;
            (void)TxDeserializer::Deserialize(tx, modified.data(), modified.size());
            break;
        }

        case FUZZ_TX_SERIALIZE: {
            // Deserialize then serialize back
            Transaction tx;
            if (TxDeserializer::Deserialize(tx, payload, payload_size)) {
                try {
                    auto serialized = tx.Serialize(true);
                    (void)serialized.size();

                    auto hex = tx.SerializeHex(true);
                    (void)hex.size();
                } catch (...) {
                    // Swallow serialization errors
                }
            }
            break;
        }

        case FUZZ_TX_METHODS: {
            // Test various Transaction methods
            Transaction tx;
            if (TxDeserializer::Deserialize(tx, payload, payload_size)) {
                try {
                    // These should not crash regardless of input
                    (void)tx.IsCoinbase();
                    (void)tx.IsLegacy();
                    (void)tx.HasWitness();
                    (void)tx.IsSegWitV0();
                    (void)tx.IsTaproot();
                    (void)tx.HasConfidentialOutputs();
                    (void)tx.CountConfidentialOutputs();

                    // Size calculations
                    (void)tx.GetSize();
                    (void)tx.GetBaseSize();
                    (void)tx.GetWeight();
                    (void)tx.GetVirtualSize();

                    // Hash calculations (may throw)
                    try {
                        (void)tx.GetTxid();
                        (void)tx.GetWtxid();
                    } catch (...) {}

                    // Detect witness version
                    tx.DetectWitnessVersion();

                } catch (...) {
                    // Swallow all exceptions
                }
            }
            break;
        }
    }

    return 0;
}

// AFL++ compatible main function
#ifdef AFL_MAIN
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::vector<uint8_t> input;
        uint8_t buf[4096];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            input.insert(input.end(), buf, buf + n);
        }
        return LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) continue;

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            continue;
        }

        std::vector<uint8_t> input(st.st_size);
        read(fd, input.data(), input.size());
        close(fd);

        LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    return 0;
}
#endif
