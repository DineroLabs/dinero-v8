/**
 * Phase 36: P2P Header Parsing Fuzz Harness
 *
 * This fuzzer exercises the P2P message header parsing code with random/malformed
 * inputs to find crashes, hangs, buffer overflows, and undefined behavior.
 *
 * Targets:
 * - Magic bytes validation
 * - Command name parsing (null termination, garbage)
 * - Payload length sanity checks
 * - Checksum validation
 * - Message frame creation
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_p2p_header.cpp ../src/daemon/p2p_message.cpp \
 *           -lssl -lcrypto -o fuzz_p2p_header
 *
 * Run:
 *   ./fuzz_p2p_header corpus/p2p_header/ -max_len=1024
 */

#include "daemon/p2p_message.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#include <memory>

using namespace dinero;

// Fuzz modes
enum P2PFuzzMode : uint8_t {
    FUZZ_HEADER_PARSE = 0,      // Parse raw header bytes
    FUZZ_MESSAGE_FRAME = 1,     // Test createMessageFrame
    FUZZ_CHECKSUM = 2,          // Test checksum calculation
    FUZZ_FACTORY = 3,           // Test P2PMessage::createFromData
    FUZZ_VERSION_MSG = 4,       // Test VersionMessage deserialize
    FUZZ_INV_MSG = 5,           // Test InvMessage deserialize
    FUZZ_HEADERS_MSG = 6,       // Test HeadersMessage deserialize
};

// Safe header parsing function
bool parseMessageHeader(const uint8_t* data, size_t size, MessageHeader& header) {
    if (size < MESSAGE_HEADER_SIZE) {
        return false;
    }

    // Parse magic bytes (4 bytes, little-endian)
    memcpy(&header.magic, data, 4);

    // Parse command (12 bytes, null-terminated string)
    memcpy(header.command, data + 4, COMMAND_SIZE);

    // Ensure null termination for safety
    header.command[COMMAND_SIZE - 1] = '\0';

    // Parse payload length (4 bytes, little-endian)
    memcpy(&header.length, data + 16, 4);

    // Parse checksum (4 bytes)
    memcpy(&header.checksum, data + 20, 4);

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
        if (c == '\0') break;  // Null terminator is OK
        if (c < 0x20 || c > 0x7E) {
            return false;  // Non-printable character
        }
    }

    return true;
}

// Extract command string safely
std::string getCommand(const MessageHeader& header) {
    std::string cmd;
    for (size_t i = 0; i < COMMAND_SIZE && header.command[i] != '\0'; ++i) {
        cmd += header.command[i];
    }
    return cmd;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Minimum viable input
    if (size < 1) {
        return 0;
    }

    // Parse fuzz mode from first byte
    P2PFuzzMode mode = static_cast<P2PFuzzMode>(data[0] % 7);
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    switch (mode) {
        case FUZZ_HEADER_PARSE: {
            // Fuzz header parsing
            if (payload_size >= MESSAGE_HEADER_SIZE) {
                MessageHeader header;
                if (parseMessageHeader(payload, payload_size, header)) {
                    // Try to validate
                    (void)validateHeader(header);
                    (void)getCommand(header);
                }
            }
            break;
        }

        case FUZZ_MESSAGE_FRAME: {
            // Fuzz message frame creation
            if (payload_size < 2) break;

            // Extract command length and command
            uint8_t cmd_len = payload[0] % 13;  // Max COMMAND_SIZE
            if (payload_size < 1 + cmd_len) break;

            std::string command(reinterpret_cast<const char*>(payload + 1), cmd_len);

            // Use rest as payload
            std::vector<uint8_t> msg_payload(payload + 1 + cmd_len, payload + payload_size);

            // Limit payload to reasonable size
            if (msg_payload.size() > 10000) {
                msg_payload.resize(10000);
            }

            try {
                auto frame = P2PMessage::createMessageFrame(command, msg_payload);
                (void)frame.size();
            } catch (...) {
                // Swallow exceptions - we're looking for crashes
            }
            break;
        }

        case FUZZ_CHECKSUM: {
            // Fuzz checksum calculation
            std::vector<uint8_t> checksum_data(payload, payload + payload_size);
            if (checksum_data.size() > 100000) {
                checksum_data.resize(100000);  // Limit to prevent OOM
            }

            try {
                uint32_t checksum = P2PMessage::calculateChecksum(checksum_data);
                (void)checksum;
            } catch (...) {
                // Swallow
            }
            break;
        }

        case FUZZ_FACTORY: {
            // Fuzz P2PMessage factory
            if (payload_size < 2) break;

            uint8_t cmd_len = payload[0] % 13;
            if (payload_size < 1 + cmd_len) break;

            std::string command(reinterpret_cast<const char*>(payload + 1), cmd_len);
            std::vector<uint8_t> msg_payload(payload + 1 + cmd_len, payload + payload_size);

            if (msg_payload.size() > MAX_MESSAGE_SIZE) {
                msg_payload.resize(MAX_MESSAGE_SIZE);
            }

            try {
                auto msg = P2PMessage::createFromData(command, msg_payload);
                if (msg) {
                    (void)msg->getCommand();
                    (void)msg->serialize();
                }
            } catch (...) {
                // Swallow
            }
            break;
        }

        case FUZZ_VERSION_MSG: {
            // Fuzz VersionMessage deserialize
            std::vector<uint8_t> msg_data(payload, payload + payload_size);

            VersionMessage version_msg;
            try {
                bool result = version_msg.deserialize(msg_data);
                if (result) {
                    (void)version_msg.version;
                    (void)version_msg.user_agent;
                    (void)version_msg.start_height;
                }
            } catch (...) {
                // Swallow
            }
            break;
        }

        case FUZZ_INV_MSG: {
            // Fuzz InvMessage deserialize
            std::vector<uint8_t> msg_data(payload, payload + payload_size);

            InvMessage inv_msg;
            try {
                bool result = inv_msg.deserialize(msg_data);
                if (result) {
                    (void)inv_msg.inventory.size();
                }
            } catch (...) {
                // Swallow
            }
            break;
        }

        case FUZZ_HEADERS_MSG: {
            // Fuzz HeadersMessage deserialize
            std::vector<uint8_t> msg_data(payload, payload + payload_size);

            HeadersMessage headers_msg;
            try {
                bool result = headers_msg.deserialize(msg_data);
                if (result) {
                    (void)headers_msg.headers.size();
                }
            } catch (...) {
                // Swallow
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
        // Read from stdin (AFL mode)
        std::vector<uint8_t> input;
        uint8_t buf[4096];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            input.insert(input.end(), buf, buf + n);
        }
        return LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    // File mode
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
