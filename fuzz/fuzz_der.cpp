/**
 * Phase 27: DER Signature Parser Fuzz Harness
 *
 * This fuzzer exercises the DER signature parsing and validation code
 * to find edge cases that could cause incorrect signature validation.
 *
 * Targets:
 * - IsValidSignatureEncoding()
 * - CheckECDSASignature() DER parsing path
 * - Low-S value validation
 *
 * Build with:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_der.cpp -L../build -ldinero_consensus -lsecp256k1 \
 *           -o fuzz_der
 *
 * Run:
 *   ./fuzz_der corpus_der/ -max_len=200
 */

#include "consensus/script_interpreter.h"
#include <cstdint>
#include <cstddef>
#include <vector>

using namespace dinero::consensus;

// Verification flags for testing
static const uint32_t FLAG_COMBOS[] = {
    0,
    SCRIPT_VERIFY_DERSIG,
    SCRIPT_VERIFY_LOW_S,
    SCRIPT_VERIFY_STRICTENC,
    SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_LOW_S,
    SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_STRICTENC,
    SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_LOW_S | SCRIPT_VERIFY_STRICTENC,
    SCRIPT_VERIFY_NULLFAIL,
    SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_NULLFAIL,
};
static const size_t NUM_FLAG_COMBOS = sizeof(FLAG_COMBOS) / sizeof(FLAG_COMBOS[0]);

// Valid compressed public keys for testing (33 bytes)
static const uint8_t VALID_PUBKEY_1[] = {
    0x02, // Compressed, even Y
    0x79, 0xBE, 0x66, 0x7E, 0xF9, 0xDC, 0xBB, 0xAC,
    0x55, 0xA0, 0x62, 0x95, 0xCE, 0x87, 0x0B, 0x07,
    0x02, 0x9B, 0xFC, 0xDB, 0x2D, 0xCE, 0x28, 0xD9,
    0x59, 0xF2, 0x81, 0x5B, 0x16, 0xF8, 0x17, 0x98
};

static const uint8_t VALID_PUBKEY_2[] = {
    0x03, // Compressed, odd Y
    0x79, 0xBE, 0x66, 0x7E, 0xF9, 0xDC, 0xBB, 0xAC,
    0x55, 0xA0, 0x62, 0x95, 0xCE, 0x87, 0x0B, 0x07,
    0x02, 0x9B, 0xFC, 0xDB, 0x2D, 0xCE, 0x28, 0xD9,
    0x59, 0xF2, 0x81, 0x5B, 0x16, 0xF8, 0x17, 0x98
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Need at least mode byte + sighash type
    if (size < 2) {
        return 0;
    }

    // First byte selects test mode and flag combination
    uint8_t mode = data[0];
    uint8_t flag_idx = data[1] % NUM_FLAG_COMBOS;
    uint32_t flags = FLAG_COMBOS[flag_idx];

    // Rest is potential signature data
    std::vector<uint8_t> sig(data + 2, data + size);

    // Test 1: IsValidSignatureEncoding (BIP 66)
    // This function checks DER validity without verifying the signature
    bool valid_encoding = IsValidSignatureEncoding(sig);
    (void)valid_encoding;

    // Test 2: Full signature verification with valid pubkey
    if (size >= 10) {  // Minimum for meaningful DER sig
        // Create a 32-byte sighash (transaction hash)
        std::vector<uint8_t> sighash(32);
        for (int i = 0; i < 32; i++) {
            sighash[i] = static_cast<uint8_t>((mode + i) ^ 0x55);
        }

        // Try verification with both pubkey types
        const uint8_t* pubkey_data = (mode & 1) ? VALID_PUBKEY_1 : VALID_PUBKEY_2;
        std::vector<uint8_t> pubkey(pubkey_data, pubkey_data + 33);

        // Check signature (result doesn't matter, looking for crashes)
        CheckECDSASignature(sig, pubkey, sighash, flags);
    }

    // Test 3: Various malformed signature structures
    if (mode & 0x10) {
        // Try with empty signature
        std::vector<uint8_t> empty_sig;
        IsValidSignatureEncoding(empty_sig);

        // Try with just sighash type
        std::vector<uint8_t> sighash_only = {0x01};
        IsValidSignatureEncoding(sighash_only);

        // Try with minimal structure
        std::vector<uint8_t> minimal = {0x30, 0x00, 0x01};  // Empty sequence + sighash
        IsValidSignatureEncoding(minimal);
    }

    // Test 4: Boundary length signatures
    if (mode & 0x20) {
        // Maximum length DER signature (73 bytes)
        if (size >= 75) {
            std::vector<uint8_t> max_sig(data + 2, data + 75);
            max_sig[1] = 70;  // Force length to max
            IsValidSignatureEncoding(max_sig);
        }

        // One byte over maximum
        if (size >= 76) {
            std::vector<uint8_t> over_max(data + 2, data + 76);
            IsValidSignatureEncoding(over_max);
        }
    }

    // Test 5: R and S value edge cases
    if (mode & 0x40 && size >= 20) {
        std::vector<uint8_t> edge_sig = {
            0x30,           // Sequence
            static_cast<uint8_t>(size > 70 ? 70 : size - 3),  // Length
            0x02,           // Integer (R)
            0x01,           // R length
            data[2],        // R value (from fuzz input)
            0x02,           // Integer (S)
            0x01,           // S length
            data[3],        // S value (from fuzz input)
            0x01            // SIGHASH_ALL
        };
        IsValidSignatureEncoding(edge_sig);

        // Test with negative R (should fail)
        edge_sig[4] = 0x80;  // Negative
        IsValidSignatureEncoding(edge_sig);

        // Test with zero R (should fail)
        edge_sig[4] = 0x00;
        IsValidSignatureEncoding(edge_sig);
    }

    // Test 6: Verify high-S detection
    if (mode & 0x80 && size >= 40) {
        // Construct signature with high S value
        std::vector<uint8_t> high_s_sig = {
            0x30, 0x44,     // Sequence, 68 bytes
            0x02, 0x20,     // Integer (R), 32 bytes
        };

        // Add 32 bytes of R
        for (int i = 0; i < 32; i++) {
            high_s_sig.push_back(data[2 + i % (size - 2)]);
        }

        high_s_sig.push_back(0x02);  // Integer (S)
        high_s_sig.push_back(0x20);  // 32 bytes

        // Add high S value (> order/2)
        high_s_sig.push_back(0x80);  // First byte > 0x7F makes it high
        for (int i = 1; i < 32; i++) {
            high_s_sig.push_back(data[2 + (i + 32) % (size - 2)]);
        }

        high_s_sig.push_back(0x01);  // SIGHASH_ALL

        IsValidSignatureEncoding(high_s_sig);

        // Also test through CheckECDSASignature
        std::vector<uint8_t> sighash(32, 0x42);
        std::vector<uint8_t> pubkey(VALID_PUBKEY_1, VALID_PUBKEY_1 + 33);
        CheckECDSASignature(high_s_sig, pubkey, sighash, SCRIPT_VERIFY_LOW_S);
    }

    return 0;
}

#ifdef AFL_MAIN
// AFL++ compatible main function
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
