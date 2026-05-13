/**
 * Phase 36: VARINT Parsing Fuzz Harness
 *
 * VARINT is historically the most fragile part of Bitcoin serialization.
 * This fuzzer exercises VARINT parsing with edge cases to find:
 * - Infinite loops
 * - Integer overflows
 * - Buffer over-reads
 * - Non-canonical encodings
 *
 * Bitcoin VARINT format:
 *   0x00-0xFC:       1 byte  (value = byte)
 *   0xFD + 2 bytes:  3 bytes (value = uint16)
 *   0xFE + 4 bytes:  5 bytes (value = uint32)
 *   0xFF + 8 bytes:  9 bytes (value = uint64)
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_varint.cpp -o fuzz_varint
 *
 * Run:
 *   ./fuzz_varint corpus/varint/ -max_len=64
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <limits>

// VARINT reading implementation (mirrors P2PMessage::readVarInt)
// Returns the decoded value and advances pos
uint64_t readVarInt(const uint8_t* data, size_t size, size_t& pos) {
    if (pos >= size) {
        throw std::out_of_range("VARINT: buffer underflow at start");
    }

    uint8_t first = data[pos++];

    if (first < 0xFD) {
        // 1-byte value
        return first;
    } else if (first == 0xFD) {
        // 3-byte encoding (uint16)
        if (pos + 2 > size) {
            throw std::out_of_range("VARINT: buffer underflow for uint16");
        }
        uint16_t value = static_cast<uint16_t>(data[pos]) |
                         (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;

        // Check for non-canonical encoding (value could fit in 1 byte)
        if (value < 0xFD) {
            // Non-canonical but parse anyway (fuzzer wants crashes, not rejections)
        }
        return value;
    } else if (first == 0xFE) {
        // 5-byte encoding (uint32)
        if (pos + 4 > size) {
            throw std::out_of_range("VARINT: buffer underflow for uint32");
        }
        uint32_t value = static_cast<uint32_t>(data[pos]) |
                         (static_cast<uint32_t>(data[pos + 1]) << 8) |
                         (static_cast<uint32_t>(data[pos + 2]) << 16) |
                         (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return value;
    } else {
        // 0xFF: 9-byte encoding (uint64)
        if (pos + 8 > size) {
            throw std::out_of_range("VARINT: buffer underflow for uint64");
        }
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
        }
        pos += 8;
        return value;
    }
}

// VARINT writing implementation
void writeVarInt(std::vector<uint8_t>& out, uint64_t value) {
    if (value < 0xFD) {
        out.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    } else if (value <= 0xFFFFFFFF) {
        out.push_back(0xFE);
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
        }
    } else {
        out.push_back(0xFF);
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }
}

// Check if VARINT is canonical (shortest encoding)
bool isCanonicalVarInt(const uint8_t* data, size_t size) {
    if (size == 0) return false;

    uint8_t first = data[0];

    if (first < 0xFD) {
        return true;  // 1-byte is always canonical
    } else if (first == 0xFD) {
        if (size < 3) return false;
        uint16_t value = static_cast<uint16_t>(data[1]) |
                         (static_cast<uint16_t>(data[2]) << 8);
        return value >= 0xFD;  // Must be >= 0xFD to require 3 bytes
    } else if (first == 0xFE) {
        if (size < 5) return false;
        uint32_t value = static_cast<uint32_t>(data[1]) |
                         (static_cast<uint32_t>(data[2]) << 8) |
                         (static_cast<uint32_t>(data[3]) << 16) |
                         (static_cast<uint32_t>(data[4]) << 24);
        return value > 0xFFFF;  // Must be > 0xFFFF to require 5 bytes
    } else {
        if (size < 9) return false;
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(data[1 + i]) << (8 * i);
        }
        return value > 0xFFFFFFFF;  // Must be > 0xFFFFFFFF to require 9 bytes
    }
}

// Fuzz modes
enum VarIntFuzzMode : uint8_t {
    FUZZ_SINGLE_READ = 0,       // Read single VARINT
    FUZZ_MULTIPLE_READ = 1,     // Read multiple VARINTs in sequence
    FUZZ_ROUNDTRIP = 2,         // Read then write, verify match
    FUZZ_CANONICAL_CHECK = 3,   // Check canonical encoding
    FUZZ_OVERFLOW = 4,          // Test overflow edge cases
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }

    VarIntFuzzMode mode = static_cast<VarIntFuzzMode>(data[0] % 5);
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    switch (mode) {
        case FUZZ_SINGLE_READ: {
            // Read a single VARINT
            size_t pos = 0;
            try {
                uint64_t value = readVarInt(payload, payload_size, pos);
                (void)value;
            } catch (const std::out_of_range&) {
                // Expected for malformed input
            } catch (...) {
                // Other exceptions are suspicious but not crashes
            }
            break;
        }

        case FUZZ_MULTIPLE_READ: {
            // Read multiple VARINTs until exhausted
            size_t pos = 0;
            int count = 0;
            const int MAX_READS = 1000;  // Prevent infinite loops

            while (pos < payload_size && count < MAX_READS) {
                try {
                    uint64_t value = readVarInt(payload, payload_size, pos);
                    (void)value;
                    count++;
                } catch (const std::out_of_range&) {
                    break;  // Normal termination
                } catch (...) {
                    break;
                }
            }
            break;
        }

        case FUZZ_ROUNDTRIP: {
            // Read VARINT, write it back, verify match
            size_t pos = 0;
            try {
                uint64_t value = readVarInt(payload, payload_size, pos);

                // Write it back
                std::vector<uint8_t> written;
                writeVarInt(written, value);

                // Read what we wrote
                size_t pos2 = 0;
                uint64_t value2 = readVarInt(written.data(), written.size(), pos2);

                // Values must match
                if (value != value2) {
                    // This would be a bug!
                    __builtin_trap();
                }
            } catch (const std::out_of_range&) {
                // Expected
            } catch (...) {
                // Unexpected
            }
            break;
        }

        case FUZZ_CANONICAL_CHECK: {
            // Check canonical encoding
            (void)isCanonicalVarInt(payload, payload_size);
            break;
        }

        case FUZZ_OVERFLOW: {
            // Test edge cases near overflow boundaries
            // Use fuzz data to construct specific test values
            if (payload_size >= 8) {
                uint64_t test_value = 0;
                for (int i = 0; i < 8; ++i) {
                    test_value |= static_cast<uint64_t>(payload[i]) << (8 * i);
                }

                std::vector<uint8_t> written;
                writeVarInt(written, test_value);

                size_t pos = 0;
                try {
                    uint64_t read_value = readVarInt(written.data(), written.size(), pos);
                    if (read_value != test_value) {
                        __builtin_trap();  // Bug found!
                    }
                } catch (...) {
                    // Shouldn't happen for valid writes
                    __builtin_trap();
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
