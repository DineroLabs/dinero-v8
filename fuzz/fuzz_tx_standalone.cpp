/**
 * Phase 36: Transaction Deserialization Fuzz Harness (Standalone)
 *
 * Self-contained fuzzer for transaction parsing.
 * Tests the critical deserialization path without external dependencies.
 *
 * Build:
 *   clang++ -std=c++17 -O2 -g -DAFL_MAIN fuzz_tx_standalone.cpp -o fuzz_tx
 *
 * Run:
 *   echo -n "test" | ./fuzz_tx
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>

// Simplified transaction structures for fuzzing
struct TxOutPoint {
    uint8_t txid[32];
    uint32_t vout;
};

struct TxInput {
    TxOutPoint prevout;
    std::vector<uint8_t> scriptSig;
    uint32_t sequence;
    std::vector<std::vector<uint8_t>> witness;
};

struct TxOutput {
    uint64_t value;
    std::vector<uint8_t> scriptPubKey;
};

struct Transaction {
    int32_t version;
    std::vector<TxInput> vin;
    std::vector<TxOutput> vout;
    uint32_t lockTime;
    bool is_segwit;
};

// VARINT reading
bool readVarInt(const uint8_t* data, size_t size, size_t& pos, uint64_t& value) {
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

// Read uint32 little-endian
bool readUint32(const uint8_t* data, size_t size, size_t& pos, uint32_t& value) {
    if (pos + 4 > size) return false;
    value = static_cast<uint32_t>(data[pos]) |
            (static_cast<uint32_t>(data[pos + 1]) << 8) |
            (static_cast<uint32_t>(data[pos + 2]) << 16) |
            (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

// Read uint64 little-endian
bool readUint64(const uint8_t* data, size_t size, size_t& pos, uint64_t& value) {
    if (pos + 8 > size) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
    }
    pos += 8;
    return true;
}

// Read bytes
bool readBytes(const uint8_t* data, size_t size, size_t& pos,
               std::vector<uint8_t>& out, size_t count) {
    if (pos + count > size) return false;
    out.assign(data + pos, data + pos + count);
    pos += count;
    return true;
}

// Parse transaction input
bool parseInput(const uint8_t* data, size_t size, size_t& pos, TxInput& input) {
    // Previous output hash (32 bytes)
    if (pos + 32 > size) return false;
    memcpy(input.prevout.txid, data + pos, 32);
    pos += 32;

    // Previous output index
    if (!readUint32(data, size, pos, input.prevout.vout)) return false;

    // ScriptSig
    uint64_t scriptLen;
    if (!readVarInt(data, size, pos, scriptLen)) return false;
    if (scriptLen > 10000) return false;  // Sanity limit
    if (!readBytes(data, size, pos, input.scriptSig, scriptLen)) return false;

    // Sequence
    if (!readUint32(data, size, pos, input.sequence)) return false;

    return true;
}

// Parse transaction output
bool parseOutput(const uint8_t* data, size_t size, size_t& pos, TxOutput& output) {
    // Value
    if (!readUint64(data, size, pos, output.value)) return false;

    // ScriptPubKey
    uint64_t scriptLen;
    if (!readVarInt(data, size, pos, scriptLen)) return false;
    if (scriptLen > 10000) return false;  // Sanity limit
    if (!readBytes(data, size, pos, output.scriptPubKey, scriptLen)) return false;

    return true;
}

// Parse full transaction
bool parseTransaction(const uint8_t* data, size_t size, Transaction& tx) {
    if (size < 10) return false;

    size_t pos = 0;

    // Version (4 bytes)
    uint32_t version;
    if (!readUint32(data, size, pos, version)) return false;
    tx.version = static_cast<int32_t>(version);

    // Check for SegWit marker (0x00 0x01)
    tx.is_segwit = false;
    if (pos + 2 <= size && data[pos] == 0x00 && data[pos + 1] == 0x01) {
        tx.is_segwit = true;
        pos += 2;
    }

    // Input count
    uint64_t inputCount;
    if (!readVarInt(data, size, pos, inputCount)) return false;
    if (inputCount > 10000) return false;  // Sanity

    // Parse inputs
    tx.vin.resize(inputCount);
    for (uint64_t i = 0; i < inputCount; ++i) {
        if (!parseInput(data, size, pos, tx.vin[i])) return false;
    }

    // Output count
    uint64_t outputCount;
    if (!readVarInt(data, size, pos, outputCount)) return false;
    if (outputCount > 10000) return false;

    // Parse outputs
    tx.vout.resize(outputCount);
    for (uint64_t i = 0; i < outputCount; ++i) {
        if (!parseOutput(data, size, pos, tx.vout[i])) return false;
    }

    // Witness data (if SegWit)
    if (tx.is_segwit) {
        for (uint64_t i = 0; i < inputCount; ++i) {
            uint64_t witnessCount;
            if (!readVarInt(data, size, pos, witnessCount)) return false;
            if (witnessCount > 500) return false;

            tx.vin[i].witness.resize(witnessCount);
            for (uint64_t j = 0; j < witnessCount; ++j) {
                uint64_t witnessLen;
                if (!readVarInt(data, size, pos, witnessLen)) return false;
                if (witnessLen > 10000) return false;
                if (!readBytes(data, size, pos, tx.vin[i].witness[j], witnessLen)) return false;
            }
        }
    }

    // Locktime
    if (!readUint32(data, size, pos, tx.lockTime)) return false;

    return true;
}

// Check if transaction is coinbase
bool isCoinbase(const Transaction& tx) {
    if (tx.vin.size() != 1) return false;

    // Check for null prevout (all zeros + 0xFFFFFFFF)
    for (int i = 0; i < 32; ++i) {
        if (tx.vin[0].prevout.txid[i] != 0) return false;
    }
    return tx.vin[0].prevout.vout == 0xFFFFFFFF;
}

// Calculate transaction weight (simplified)
size_t calculateWeight(const Transaction& tx) {
    // Base weight = 4 * base_size
    // Total weight = base_weight + witness_size

    size_t base = 0;
    base += 4;  // version
    base += 1 + tx.vin.size() * (32 + 4 + 1 + 4);  // inputs (simplified)
    base += 1 + tx.vout.size() * (8 + 1);  // outputs (simplified)
    base += 4;  // locktime

    size_t witness = 0;
    if (tx.is_segwit) {
        witness += 2;  // marker + flag
        for (const auto& input : tx.vin) {
            witness += 1;  // witness count
            for (const auto& w : input.witness) {
                witness += 1 + w.size();
            }
        }
    }

    return base * 4 + witness;
}

enum FuzzMode : uint8_t {
    FUZZ_PARSE = 0,
    FUZZ_LEGACY = 1,
    FUZZ_SEGWIT = 2,
    FUZZ_ANALYSIS = 3,
    FUZZ_STRESS = 4,
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;

    FuzzMode mode = static_cast<FuzzMode>(data[0] % 5);
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    switch (mode) {
        case FUZZ_PARSE: {
            Transaction tx;
            if (parseTransaction(payload, payload_size, tx)) {
                (void)tx.version;
                (void)tx.vin.size();
                (void)tx.vout.size();
                (void)tx.lockTime;
            }
            break;
        }

        case FUZZ_LEGACY: {
            // Force non-SegWit parsing
            std::vector<uint8_t> modified(payload, payload + payload_size);
            if (modified.size() >= 6) {
                if (modified[4] == 0x00 && modified[5] == 0x01) {
                    modified[5] = 0x02;  // Break marker
                }
            }
            Transaction tx;
            (void)parseTransaction(modified.data(), modified.size(), tx);
            break;
        }

        case FUZZ_SEGWIT: {
            // Force SegWit parsing
            std::vector<uint8_t> modified(payload, payload + payload_size);
            if (modified.size() >= 6) {
                modified[4] = 0x00;
                modified[5] = 0x01;
            }
            Transaction tx;
            (void)parseTransaction(modified.data(), modified.size(), tx);
            break;
        }

        case FUZZ_ANALYSIS: {
            Transaction tx;
            if (parseTransaction(payload, payload_size, tx)) {
                (void)isCoinbase(tx);
                (void)calculateWeight(tx);

                // Access all data
                for (const auto& input : tx.vin) {
                    (void)input.prevout.vout;
                    (void)input.scriptSig.size();
                    (void)input.sequence;
                    for (const auto& w : input.witness) {
                        (void)w.size();
                    }
                }
                for (const auto& output : tx.vout) {
                    (void)output.value;
                    (void)output.scriptPubKey.size();
                }
            }
            break;
        }

        case FUZZ_STRESS: {
            // Parse multiple transactions from stream
            size_t pos = 0;
            int count = 0;
            while (pos < payload_size && count < 100) {
                Transaction tx;
                size_t remaining = payload_size - pos;
                if (parseTransaction(payload + pos, remaining, tx)) {
                    count++;
                    // Advance by minimum tx size
                    pos += 60;
                } else {
                    break;
                }
            }
            break;
        }
    }

    return 0;
}

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
