/**
 * Phase 36: P2P Header Parsing Fuzz Harness (Standalone)
 *
 * Self-contained fuzzer for P2P message header parsing.
 * No external library dependencies - all parsing logic inlined.
 *
 * Build:
 *   clang++ -std=c++17 -O2 -g -DAFL_MAIN fuzz_p2p_header_standalone.cpp -o fuzz_p2p_header
 *
 * Run with AFL:
 *   afl-fuzz -i corpus/p2p_header -o findings ./fuzz_p2p_header @@
 *
 * Run standalone:
 *   echo -n "test" | ./fuzz_p2p_header
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#include <stdexcept>

// P2P constants. Network magic is generated from chainparams so this
// standalone harness cannot drift from the daemon/seeder wire identity.
#include "../seeder/include/dinero/seeder/network_constants_generated.h"
const uint32_t MAGIC_BYTES = dinero::seeder::kMagicMainnet;
const size_t MESSAGE_HEADER_SIZE = 24;
const size_t MAX_MESSAGE_SIZE = 4 * 1024 * 1024;
const size_t COMMAND_SIZE = 12;

// P2P message header structure
struct MessageHeader {
    uint32_t magic;
    char command[COMMAND_SIZE];
    uint32_t length;
    uint32_t checksum;

    MessageHeader() : magic(0), length(0), checksum(0) {
        memset(command, 0, COMMAND_SIZE);
    }
};

// SHA256 double hash for checksum (simplified - just XOR for fuzzing purposes)
uint32_t calculateChecksum(const uint8_t* data, size_t size) {
    uint32_t hash = 0;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint32_t>(data[i]) << ((i % 4) * 8);
        hash = (hash << 5) | (hash >> 27);  // Rotate
    }
    return hash;
}

// Parse message header from raw bytes
bool parseMessageHeader(const uint8_t* data, size_t size, MessageHeader& header) {
    if (size < MESSAGE_HEADER_SIZE) {
        return false;
    }

    // Magic bytes (4 bytes, little-endian)
    header.magic = static_cast<uint32_t>(data[0]) |
                   (static_cast<uint32_t>(data[1]) << 8) |
                   (static_cast<uint32_t>(data[2]) << 16) |
                   (static_cast<uint32_t>(data[3]) << 24);

    // Command (12 bytes)
    memcpy(header.command, data + 4, COMMAND_SIZE);
    header.command[COMMAND_SIZE - 1] = '\0';  // Ensure null termination

    // Length (4 bytes, little-endian)
    header.length = static_cast<uint32_t>(data[16]) |
                    (static_cast<uint32_t>(data[17]) << 8) |
                    (static_cast<uint32_t>(data[18]) << 16) |
                    (static_cast<uint32_t>(data[19]) << 24);

    // Checksum (4 bytes)
    header.checksum = static_cast<uint32_t>(data[20]) |
                      (static_cast<uint32_t>(data[21]) << 8) |
                      (static_cast<uint32_t>(data[22]) << 16) |
                      (static_cast<uint32_t>(data[23]) << 24);

    return true;
}

// Validate header fields
bool validateHeader(const MessageHeader& header) {
    // Check magic bytes
    if (header.magic != MAGIC_BYTES) {
        return false;
    }

    // Check payload length limits
    if (header.length > MAX_MESSAGE_SIZE) {
        return false;
    }

    // Validate command is printable ASCII or null
    for (size_t i = 0; i < COMMAND_SIZE; ++i) {
        char c = header.command[i];
        if (c == '\0') break;
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }

    return true;
}

// Extract command string
std::string getCommand(const MessageHeader& header) {
    std::string cmd;
    for (size_t i = 0; i < COMMAND_SIZE && header.command[i] != '\0'; ++i) {
        cmd += header.command[i];
    }
    return cmd;
}

// Serialize header to bytes
std::vector<uint8_t> serializeHeader(const MessageHeader& header) {
    std::vector<uint8_t> result(MESSAGE_HEADER_SIZE);

    // Magic
    result[0] = header.magic & 0xFF;
    result[1] = (header.magic >> 8) & 0xFF;
    result[2] = (header.magic >> 16) & 0xFF;
    result[3] = (header.magic >> 24) & 0xFF;

    // Command
    memcpy(result.data() + 4, header.command, COMMAND_SIZE);

    // Length
    result[16] = header.length & 0xFF;
    result[17] = (header.length >> 8) & 0xFF;
    result[18] = (header.length >> 16) & 0xFF;
    result[19] = (header.length >> 24) & 0xFF;

    // Checksum
    result[20] = header.checksum & 0xFF;
    result[21] = (header.checksum >> 8) & 0xFF;
    result[22] = (header.checksum >> 16) & 0xFF;
    result[23] = (header.checksum >> 24) & 0xFF;

    return result;
}

// VARINT parsing (reused from fuzz_varint.cpp)
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

// Fuzz modes
enum FuzzMode : uint8_t {
    FUZZ_HEADER_PARSE = 0,
    FUZZ_HEADER_VALIDATE = 1,
    FUZZ_HEADER_ROUNDTRIP = 2,
    FUZZ_CHECKSUM = 3,
    FUZZ_FULL_MESSAGE = 4,
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }

    FuzzMode mode = static_cast<FuzzMode>(data[0] % 5);
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    switch (mode) {
        case FUZZ_HEADER_PARSE: {
            MessageHeader header;
            if (parseMessageHeader(payload, payload_size, header)) {
                (void)header.magic;
                (void)header.length;
                (void)header.checksum;
                (void)getCommand(header);
            }
            break;
        }

        case FUZZ_HEADER_VALIDATE: {
            MessageHeader header;
            if (parseMessageHeader(payload, payload_size, header)) {
                (void)validateHeader(header);
            }
            break;
        }

        case FUZZ_HEADER_ROUNDTRIP: {
            MessageHeader header;
            if (parseMessageHeader(payload, payload_size, header)) {
                auto serialized = serializeHeader(header);

                MessageHeader header2;
                if (parseMessageHeader(serialized.data(), serialized.size(), header2)) {
                    // Verify roundtrip
                    if (header.magic != header2.magic ||
                        header.length != header2.length ||
                        header.checksum != header2.checksum) {
                        __builtin_trap();  // Bug!
                    }
                }
            }
            break;
        }

        case FUZZ_CHECKSUM: {
            if (payload_size > 0 && payload_size <= 100000) {
                uint32_t checksum = calculateChecksum(payload, payload_size);
                (void)checksum;
            }
            break;
        }

        case FUZZ_FULL_MESSAGE: {
            // Parse header + payload
            MessageHeader header;
            if (parseMessageHeader(payload, payload_size, header)) {
                if (validateHeader(header)) {
                    // Check if we have enough data for payload
                    if (payload_size >= MESSAGE_HEADER_SIZE + header.length) {
                        const uint8_t* msg_payload = payload + MESSAGE_HEADER_SIZE;
                        uint32_t computed = calculateChecksum(msg_payload, header.length);
                        (void)computed;
                    }
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
