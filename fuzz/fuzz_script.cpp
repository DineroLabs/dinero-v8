/**
 * Phase 27: Script Interpreter Fuzz Harness
 *
 * This fuzzer exercises the Bitcoin Script interpreter with random inputs
 * to find crashes, hangs, and undefined behavior.
 *
 * Targets:
 * - EvalScript() - core VM execution
 * - VerifyScript() - full script verification
 * - DER signature parsing
 * - Hash functions
 *
 * Build with:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_script.cpp -L../build -ldinero_consensus \
 *           -o fuzz_script
 *
 * Run:
 *   ./fuzz_script corpus/ -max_len=10000
 */

#include "consensus/script_interpreter.h"
#include "consensus/script.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace dinero::consensus;

// Minimum input structure:
// [1 byte: fuzz mode]
// [2 bytes: flags (little-endian)]
// [2 bytes: scriptSig length]
// [N bytes: scriptSig]
// [2 bytes: scriptPubKey length]
// [M bytes: scriptPubKey]
// [remaining: witness data]

enum FuzzMode : uint8_t {
    FUZZ_EVAL_SCRIPT = 0,
    FUZZ_VERIFY_SCRIPT = 1,
    FUZZ_SCRIPT_ONLY = 2,
    FUZZ_CHECKSIG = 3,
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Minimum viable input
    if (size < 6) {
        return 0;
    }

    // Parse fuzz mode
    FuzzMode mode = static_cast<FuzzMode>(data[0] % 4);
    uint32_t flags = static_cast<uint32_t>(data[1]) | (static_cast<uint32_t>(data[2]) << 8);
    size_t offset = 3;

    // Constrain flags to valid combinations
    flags &= (SCRIPT_VERIFY_P2SH |
              SCRIPT_VERIFY_DERSIG |
              SCRIPT_VERIFY_LOW_S |
              SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY |
              SCRIPT_VERIFY_CHECKSEQUENCEVERIFY |
              SCRIPT_VERIFY_WITNESS |
              SCRIPT_VERIFY_NULLDUMMY |
              SCRIPT_VERIFY_STRICTENC |
              SCRIPT_VERIFY_MINIMALDATA |
              SCRIPT_VERIFY_NULLFAIL |
              SCRIPT_VERIFY_CLEANSTACK |
              SCRIPT_VERIFY_MINIMALIF |
              SCRIPT_VERIFY_WITNESS_PUBKEYTYPE);

    // Parse scriptSig length
    if (offset + 2 > size) return 0;
    uint16_t scriptSigLen = static_cast<uint16_t>(data[offset]) |
                            (static_cast<uint16_t>(data[offset + 1]) << 8);
    scriptSigLen = scriptSigLen % 10001;  // Limit to MAX_SCRIPT_SIZE + 1
    offset += 2;

    // Parse scriptSig
    if (offset + scriptSigLen > size) return 0;
    Script scriptSig(data + offset, data + offset + scriptSigLen);
    offset += scriptSigLen;

    // Parse scriptPubKey length
    if (offset + 2 > size) return 0;
    uint16_t scriptPubKeyLen = static_cast<uint16_t>(data[offset]) |
                               (static_cast<uint16_t>(data[offset + 1]) << 8);
    scriptPubKeyLen = scriptPubKeyLen % 10001;
    offset += 2;

    // Parse scriptPubKey
    if (offset + scriptPubKeyLen > size) return 0;
    Script scriptPubKey(data + offset, data + offset + scriptPubKeyLen);
    offset += scriptPubKeyLen;

    // Parse witness data (remaining bytes as witness stack elements)
    std::vector<std::vector<uint8_t>> witness;
    while (offset + 2 <= size) {
        uint16_t elemLen = static_cast<uint16_t>(data[offset]) |
                          (static_cast<uint16_t>(data[offset + 1]) << 8);
        elemLen = elemLen % 521;  // MAX_SCRIPT_ELEMENT_SIZE + 1
        offset += 2;

        if (offset + elemLen > size) break;
        witness.emplace_back(data + offset, data + offset + elemLen);
        offset += elemLen;

        if (witness.size() > 100) break;  // Limit witness stack size
    }

    // Create execution context with nullptr transaction
    // This limits testing to opcodes that don't need tx data (most of them)
    ScriptExecutionContext ctx(nullptr, 0, 100000000, flags);

    ScriptError error = ScriptError::OK;

    switch (mode) {
        case FUZZ_EVAL_SCRIPT: {
            // Fuzz just EvalScript with scriptPubKey
            std::vector<std::vector<uint8_t>> stack;
            EvalScript(scriptPubKey, stack, ctx, error);
            break;
        }

        case FUZZ_VERIFY_SCRIPT: {
            // Fuzz full VerifyScript (will fail on CHECKSIG but tests other paths)
            VerifyScript(scriptSig, scriptPubKey, witness, ctx, error);
            break;
        }

        case FUZZ_SCRIPT_ONLY: {
            // Fuzz EvalScript with scriptSig first, then scriptPubKey
            std::vector<std::vector<uint8_t>> stack;
            if (EvalScript(scriptSig, stack, ctx, error)) {
                EvalScript(scriptPubKey, stack, ctx, error);
            }
            break;
        }

        case FUZZ_CHECKSIG: {
            // Fuzz signature verification specifically
            // Use scriptSig as signature, scriptPubKey as pubkey
            if (scriptSigLen >= 9 && scriptPubKeyLen >= 33) {
                std::vector<uint8_t> sighash(32, 0x42);  // Dummy sighash
                std::vector<uint8_t> sig(scriptSig.begin(), scriptSig.end());
                std::vector<uint8_t> pubkey(scriptPubKey.begin(), scriptPubKey.end());
                CheckECDSASignature(sig, pubkey, sighash, flags);
            }
            break;
        }
    }

    // The result doesn't matter - we're looking for crashes/UB
    (void)error;

    return 0;
}

#ifdef AFL_MAIN
// AFL++ compatible main function
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
