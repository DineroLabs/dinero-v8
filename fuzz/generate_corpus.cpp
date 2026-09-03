/**
 * Phase 27: Corpus Generator for Script Fuzzer
 *
 * Generates initial seed corpus of valid Bitcoin scripts for fuzzing.
 * Each seed file follows the format expected by fuzz_script.cpp:
 *
 * [1 byte: fuzz mode]
 * [2 bytes: flags (little-endian)]
 * [2 bytes: scriptSig length]
 * [N bytes: scriptSig]
 * [2 bytes: scriptPubKey length]
 * [M bytes: scriptPubKey]
 * [witness data...]
 *
 * Build:
 *   clang++ -std=c++20 -O2 generate_corpus.cpp -o generate_corpus
 *
 * Run:
 *   ./generate_corpus corpus/
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <sys/stat.h>

// Script opcodes (subset needed for corpus)
enum opcode {
    OP_0 = 0x00,
    OP_PUSHDATA1 = 0x4c,
    OP_PUSHDATA2 = 0x4d,
    OP_PUSHDATA4 = 0x4e,
    OP_1NEGATE = 0x4f,
    OP_RESERVED = 0x50,
    OP_1 = 0x51,
    OP_2 = 0x52,
    OP_3 = 0x53,
    OP_4 = 0x54,
    OP_5 = 0x55,
    OP_6 = 0x56,
    OP_7 = 0x57,
    OP_8 = 0x58,
    OP_9 = 0x59,
    OP_10 = 0x5a,
    OP_11 = 0x5b,
    OP_12 = 0x5c,
    OP_13 = 0x5d,
    OP_14 = 0x5e,
    OP_15 = 0x5f,
    OP_16 = 0x60,
    OP_NOP = 0x61,
    OP_VER = 0x62,
    OP_IF = 0x63,
    OP_NOTIF = 0x64,
    OP_VERIF = 0x65,
    OP_VERNOTIF = 0x66,
    OP_ELSE = 0x67,
    OP_ENDIF = 0x68,
    OP_VERIFY = 0x69,
    OP_RETURN = 0x6a,
    OP_TOALTSTACK = 0x6b,
    OP_FROMALTSTACK = 0x6c,
    OP_2DROP = 0x6d,
    OP_2DUP = 0x6e,
    OP_3DUP = 0x6f,
    OP_2OVER = 0x70,
    OP_2ROT = 0x71,
    OP_2SWAP = 0x72,
    OP_IFDUP = 0x73,
    OP_DEPTH = 0x74,
    OP_DROP = 0x75,
    OP_DUP = 0x76,
    OP_NIP = 0x77,
    OP_OVER = 0x78,
    OP_PICK = 0x79,
    OP_ROLL = 0x7a,
    OP_ROT = 0x7b,
    OP_SWAP = 0x7c,
    OP_TUCK = 0x7d,
    OP_SIZE = 0x82,
    OP_EQUAL = 0x87,
    OP_EQUALVERIFY = 0x88,
    OP_1ADD = 0x8b,
    OP_1SUB = 0x8c,
    OP_NEGATE = 0x8f,
    OP_ABS = 0x90,
    OP_NOT = 0x91,
    OP_0NOTEQUAL = 0x92,
    OP_ADD = 0x93,
    OP_SUB = 0x94,
    OP_BOOLAND = 0x9a,
    OP_BOOLOR = 0x9b,
    OP_NUMEQUAL = 0x9c,
    OP_NUMEQUALVERIFY = 0x9d,
    OP_NUMNOTEQUAL = 0x9e,
    OP_LESSTHAN = 0x9f,
    OP_GREATERTHAN = 0xa0,
    OP_LESSTHANOREQUAL = 0xa1,
    OP_GREATERTHANOREQUAL = 0xa2,
    OP_MIN = 0xa3,
    OP_MAX = 0xa4,
    OP_WITHIN = 0xa5,
    OP_RIPEMD160 = 0xa6,
    OP_SHA1 = 0xa7,
    OP_SHA256 = 0xa8,
    OP_HASH160 = 0xa9,
    OP_HASH256 = 0xaa,
    OP_CODESEPARATOR = 0xab,
    OP_CHECKSIG = 0xac,
    OP_CHECKSIGVERIFY = 0xad,
    OP_CHECKMULTISIG = 0xae,
    OP_CHECKMULTISIGVERIFY = 0xaf,
    OP_NOP1 = 0xb0,
    OP_CHECKLOCKTIMEVERIFY = 0xb1,
    OP_CHECKSEQUENCEVERIFY = 0xb2,
    OP_NOP4 = 0xb3,
    OP_NOP5 = 0xb4,
    OP_NOP6 = 0xb5,
    OP_NOP7 = 0xb6,
    OP_NOP8 = 0xb7,
    OP_NOP9 = 0xb8,
    OP_NOP10 = 0xb9,
};

// Verification flags
enum {
    SCRIPT_VERIFY_NONE = 0,
    SCRIPT_VERIFY_P2SH = (1U << 0),
    SCRIPT_VERIFY_STRICTENC = (1U << 1),
    SCRIPT_VERIFY_DERSIG = (1U << 2),
    SCRIPT_VERIFY_LOW_S = (1U << 3),
    SCRIPT_VERIFY_NULLDUMMY = (1U << 4),
    SCRIPT_VERIFY_SIGPUSHONLY = (1U << 5),
    SCRIPT_VERIFY_MINIMALDATA = (1U << 6),
    SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS = (1U << 7),
    SCRIPT_VERIFY_CLEANSTACK = (1U << 8),
    SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY = (1U << 9),
    SCRIPT_VERIFY_CHECKSEQUENCEVERIFY = (1U << 10),
    SCRIPT_VERIFY_WITNESS = (1U << 11),
    SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM = (1U << 12),
    SCRIPT_VERIFY_MINIMALIF = (1U << 13),
    SCRIPT_VERIFY_NULLFAIL = (1U << 14),
    SCRIPT_VERIFY_WITNESS_PUBKEYTYPE = (1U << 15),
};

// Fuzz modes (must match fuzz_script.cpp)
enum FuzzMode : uint8_t {
    FUZZ_EVAL_SCRIPT = 0,
    FUZZ_VERIFY_SCRIPT = 1,
    FUZZ_SCRIPT_ONLY = 2,
    FUZZ_CHECKSIG = 3,
};

// Helper to write seed file
static void WriteSeed(
    const std::string& dir,
    const std::string& name,
    uint8_t mode,
    uint16_t flags,
    const std::vector<uint8_t>& scriptSig,
    const std::vector<uint8_t>& scriptPubKey,
    const std::vector<std::vector<uint8_t>>& witness = {}
) {
    std::vector<uint8_t> data;

    // Mode
    data.push_back(mode);

    // Flags (little-endian)
    data.push_back(flags & 0xFF);
    data.push_back((flags >> 8) & 0xFF);

    // scriptSig length and data
    uint16_t sigLen = static_cast<uint16_t>(scriptSig.size());
    data.push_back(sigLen & 0xFF);
    data.push_back((sigLen >> 8) & 0xFF);
    data.insert(data.end(), scriptSig.begin(), scriptSig.end());

    // scriptPubKey length and data
    uint16_t pkLen = static_cast<uint16_t>(scriptPubKey.size());
    data.push_back(pkLen & 0xFF);
    data.push_back((pkLen >> 8) & 0xFF);
    data.insert(data.end(), scriptPubKey.begin(), scriptPubKey.end());

    // Witness stack elements
    for (const auto& elem : witness) {
        uint16_t elemLen = static_cast<uint16_t>(elem.size());
        data.push_back(elemLen & 0xFF);
        data.push_back((elemLen >> 8) & 0xFF);
        data.insert(data.end(), elem.begin(), elem.end());
    }

    // Write to file
    std::string path = dir + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        printf("  Created: %s (%zu bytes)\n", name.c_str(), data.size());
    } else {
        printf("  ERROR: Failed to create %s\n", name.c_str());
    }
}

// Push data helper
static void PushData(std::vector<uint8_t>& script, const std::vector<uint8_t>& data) {
    if (data.size() == 0) {
        script.push_back(OP_0);
    } else if (data.size() == 1 && data[0] >= 1 && data[0] <= 16) {
        script.push_back(OP_1 + data[0] - 1);
    } else if (data.size() < 76) {
        script.push_back(static_cast<uint8_t>(data.size()));
        script.insert(script.end(), data.begin(), data.end());
    } else if (data.size() < 256) {
        script.push_back(OP_PUSHDATA1);
        script.push_back(static_cast<uint8_t>(data.size()));
        script.insert(script.end(), data.begin(), data.end());
    } else {
        script.push_back(OP_PUSHDATA2);
        script.push_back(data.size() & 0xFF);
        script.push_back((data.size() >> 8) & 0xFF);
        script.insert(script.end(), data.begin(), data.end());
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <corpus_dir>\n", argv[0]);
        return 1;
    }

    std::string dir = argv[1];
    mkdir(dir.c_str(), 0755);

    printf("Generating fuzz corpus in %s...\n\n", dir.c_str());

    // =========================================================================
    // Basic Scripts
    // =========================================================================
    printf("Basic scripts:\n");

    // Empty scripts
    WriteSeed(dir, "empty_empty", FUZZ_EVAL_SCRIPT, 0, {}, {});

    // OP_TRUE
    WriteSeed(dir, "op_true", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1});

    // OP_FALSE
    WriteSeed(dir, "op_false", FUZZ_EVAL_SCRIPT, 0, {}, {OP_0});

    // Push small numbers
    WriteSeed(dir, "push_1", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1});
    WriteSeed(dir, "push_16", FUZZ_EVAL_SCRIPT, 0, {}, {OP_16});
    WriteSeed(dir, "push_1negate", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1NEGATE});

    // =========================================================================
    // Stack Operations
    // =========================================================================
    printf("\nStack operations:\n");

    // DUP
    WriteSeed(dir, "dup", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_DUP});

    // DROP
    WriteSeed(dir, "drop", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_DROP, OP_1});

    // SWAP
    WriteSeed(dir, "swap", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_2, OP_SWAP});

    // ROT
    WriteSeed(dir, "rot", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_2, OP_3, OP_ROT});

    // OVER
    WriteSeed(dir, "over", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_2, OP_OVER});

    // 2DUP
    WriteSeed(dir, "2dup", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_2, OP_2DUP});

    // DEPTH
    WriteSeed(dir, "depth", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_2, OP_3, OP_DEPTH});

    // =========================================================================
    // Arithmetic Operations
    // =========================================================================
    printf("\nArithmetic operations:\n");

    // ADD
    WriteSeed(dir, "add", FUZZ_EVAL_SCRIPT, 0, {}, {OP_2, OP_3, OP_ADD});

    // SUB
    WriteSeed(dir, "sub", FUZZ_EVAL_SCRIPT, 0, {}, {OP_5, OP_3, OP_SUB});

    // 1ADD
    WriteSeed(dir, "1add", FUZZ_EVAL_SCRIPT, 0, {}, {OP_5, OP_1ADD});

    // 1SUB
    WriteSeed(dir, "1sub", FUZZ_EVAL_SCRIPT, 0, {}, {OP_5, OP_1SUB});

    // NEGATE
    WriteSeed(dir, "negate", FUZZ_EVAL_SCRIPT, 0, {}, {OP_5, OP_NEGATE});

    // ABS
    WriteSeed(dir, "abs", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1NEGATE, OP_ABS});

    // MIN/MAX
    WriteSeed(dir, "min", FUZZ_EVAL_SCRIPT, 0, {}, {OP_3, OP_5, OP_MIN});
    WriteSeed(dir, "max", FUZZ_EVAL_SCRIPT, 0, {}, {OP_3, OP_5, OP_MAX});

    // Comparisons
    WriteSeed(dir, "lessthan", FUZZ_EVAL_SCRIPT, 0, {}, {OP_3, OP_5, OP_LESSTHAN});
    WriteSeed(dir, "greaterthan", FUZZ_EVAL_SCRIPT, 0, {}, {OP_5, OP_3, OP_GREATERTHAN});

    // =========================================================================
    // Logic Operations
    // =========================================================================
    printf("\nLogic operations:\n");

    // EQUAL
    WriteSeed(dir, "equal_true", FUZZ_EVAL_SCRIPT, 0, {}, {OP_5, OP_5, OP_EQUAL});
    WriteSeed(dir, "equal_false", FUZZ_EVAL_SCRIPT, 0, {}, {OP_5, OP_3, OP_EQUAL});

    // NOT
    WriteSeed(dir, "not_0", FUZZ_EVAL_SCRIPT, 0, {}, {OP_0, OP_NOT});
    WriteSeed(dir, "not_1", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_NOT});

    // BOOLAND
    WriteSeed(dir, "booland", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_1, OP_BOOLAND});

    // BOOLOR
    WriteSeed(dir, "boolor", FUZZ_EVAL_SCRIPT, 0, {}, {OP_0, OP_1, OP_BOOLOR});

    // =========================================================================
    // Control Flow
    // =========================================================================
    printf("\nControl flow:\n");

    // IF/ENDIF true
    WriteSeed(dir, "if_true", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_IF, OP_2, OP_ENDIF});

    // IF/ENDIF false
    WriteSeed(dir, "if_false", FUZZ_EVAL_SCRIPT, 0, {}, {OP_0, OP_IF, OP_2, OP_ENDIF, OP_1});

    // IF/ELSE/ENDIF
    WriteSeed(dir, "if_else", FUZZ_EVAL_SCRIPT, 0, {}, {OP_1, OP_IF, OP_2, OP_ELSE, OP_3, OP_ENDIF});

    // NOTIF
    WriteSeed(dir, "notif", FUZZ_EVAL_SCRIPT, 0, {}, {OP_0, OP_NOTIF, OP_2, OP_ENDIF});

    // Nested IF
    WriteSeed(dir, "nested_if", FUZZ_EVAL_SCRIPT, 0, {},
              {OP_1, OP_IF, OP_1, OP_IF, OP_2, OP_ENDIF, OP_ENDIF});

    // =========================================================================
    // Hash Operations
    // =========================================================================
    printf("\nHash operations:\n");

    // SHA256
    std::vector<uint8_t> sha256_script;
    PushData(sha256_script, {0x01, 0x02, 0x03, 0x04});
    sha256_script.push_back(OP_SHA256);
    WriteSeed(dir, "sha256", FUZZ_EVAL_SCRIPT, 0, {}, sha256_script);

    // HASH160
    std::vector<uint8_t> hash160_script;
    PushData(hash160_script, {0x01, 0x02, 0x03, 0x04});
    hash160_script.push_back(OP_HASH160);
    WriteSeed(dir, "hash160", FUZZ_EVAL_SCRIPT, 0, {}, hash160_script);

    // HASH256
    std::vector<uint8_t> hash256_script;
    PushData(hash256_script, {0x01, 0x02, 0x03, 0x04});
    hash256_script.push_back(OP_HASH256);
    WriteSeed(dir, "hash256", FUZZ_EVAL_SCRIPT, 0, {}, hash256_script);

    // RIPEMD160
    std::vector<uint8_t> ripemd160_script;
    PushData(ripemd160_script, {0x01, 0x02, 0x03, 0x04});
    ripemd160_script.push_back(OP_RIPEMD160);
    WriteSeed(dir, "ripemd160", FUZZ_EVAL_SCRIPT, 0, {}, ripemd160_script);

    // =========================================================================
    // P2PKH Pattern
    // =========================================================================
    printf("\nP2PKH patterns:\n");

    // Standard P2PKH scriptPubKey: DUP HASH160 <20 bytes> EQUALVERIFY CHECKSIG
    std::vector<uint8_t> p2pkh_pubkey = {
        OP_DUP, OP_HASH160,
        0x14,  // Push 20 bytes
        0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
        0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
        0x89, 0xab, 0xcd, 0xef,
        OP_EQUALVERIFY, OP_CHECKSIG
    };

    // Dummy signature (DER + sighash type)
    std::vector<uint8_t> dummy_sig = {
        0x30, 0x44,  // DER sequence, 68 bytes
        0x02, 0x20,  // Integer, 32 bytes (R)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
        0x02, 0x20,  // Integer, 32 bytes (S)
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
        0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
        0x01  // SIGHASH_ALL
    };

    // Compressed pubkey (33 bytes)
    std::vector<uint8_t> dummy_pubkey = {
        0x02,  // Compressed prefix
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };

    std::vector<uint8_t> p2pkh_sig;
    PushData(p2pkh_sig, dummy_sig);
    PushData(p2pkh_sig, dummy_pubkey);

    WriteSeed(dir, "p2pkh_verify", FUZZ_VERIFY_SCRIPT,
              SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_DERSIG,
              p2pkh_sig, p2pkh_pubkey);

    // =========================================================================
    // P2SH Pattern
    // =========================================================================
    printf("\nP2SH patterns:\n");

    // P2SH scriptPubKey: HASH160 <20 bytes> EQUAL
    std::vector<uint8_t> p2sh_pubkey = {
        OP_HASH160,
        0x14,  // Push 20 bytes
        0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
        0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
        0x89, 0xab, 0xcd, 0xef,
        OP_EQUAL
    };

    // Simple redeem script: OP_1
    std::vector<uint8_t> p2sh_sig;
    PushData(p2sh_sig, {OP_1});  // Push redeem script

    WriteSeed(dir, "p2sh_verify", FUZZ_VERIFY_SCRIPT,
              SCRIPT_VERIFY_P2SH, p2sh_sig, p2sh_pubkey);

    // =========================================================================
    // OP_RETURN (Data carrier)
    // =========================================================================
    printf("\nOP_RETURN:\n");

    std::vector<uint8_t> op_return_script = {OP_RETURN};
    // Add some data
    for (int i = 0; i < 40; i++) {
        op_return_script.push_back(static_cast<uint8_t>(i));
    }
    WriteSeed(dir, "op_return", FUZZ_EVAL_SCRIPT, 0, {}, op_return_script);

    // =========================================================================
    // Locktime operations
    // =========================================================================
    printf("\nLocktime operations:\n");

    // CHECKLOCKTIMEVERIFY
    std::vector<uint8_t> cltv_script;
    PushData(cltv_script, {0x00, 0x00, 0x01, 0x00});  // locktime value
    cltv_script.push_back(OP_CHECKLOCKTIMEVERIFY);
    cltv_script.push_back(OP_DROP);
    cltv_script.push_back(OP_1);
    WriteSeed(dir, "cltv", FUZZ_EVAL_SCRIPT,
              SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY, {}, cltv_script);

    // CHECKSEQUENCEVERIFY
    std::vector<uint8_t> csv_script;
    PushData(csv_script, {0x01, 0x00, 0x00, 0x00});  // sequence value
    csv_script.push_back(OP_CHECKSEQUENCEVERIFY);
    csv_script.push_back(OP_DROP);
    csv_script.push_back(OP_1);
    WriteSeed(dir, "csv", FUZZ_EVAL_SCRIPT,
              SCRIPT_VERIFY_CHECKSEQUENCEVERIFY, {}, csv_script);

    // =========================================================================
    // Multisig
    // =========================================================================
    printf("\nMultisig:\n");

    // 1-of-1 multisig
    std::vector<uint8_t> multisig_1of1 = {OP_1};
    PushData(multisig_1of1, dummy_pubkey);
    multisig_1of1.push_back(OP_1);
    multisig_1of1.push_back(OP_CHECKMULTISIG);

    std::vector<uint8_t> multisig_sig = {OP_0};  // NULLDUMMY
    PushData(multisig_sig, dummy_sig);

    WriteSeed(dir, "multisig_1of1", FUZZ_VERIFY_SCRIPT,
              SCRIPT_VERIFY_NULLDUMMY, multisig_sig, multisig_1of1);

    // =========================================================================
    // Witness programs (SegWit)
    // =========================================================================
    printf("\nWitness programs:\n");

    // P2WPKH: version 0, 20-byte program
    std::vector<uint8_t> p2wpkh = {
        0x00,  // OP_0 (witness version)
        0x14,  // Push 20 bytes
        0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
        0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67,
        0x89, 0xab, 0xcd, 0xef
    };

    std::vector<std::vector<uint8_t>> p2wpkh_witness = {dummy_sig, dummy_pubkey};
    WriteSeed(dir, "p2wpkh", FUZZ_VERIFY_SCRIPT,
              SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_P2SH,
              {}, p2wpkh, p2wpkh_witness);

    // P2WSH: version 0, 32-byte program
    std::vector<uint8_t> p2wsh = {
        0x00,  // OP_0 (witness version)
        0x20,  // Push 32 bytes
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };

    std::vector<std::vector<uint8_t>> p2wsh_witness = {{OP_1}};  // Simple witness script
    WriteSeed(dir, "p2wsh", FUZZ_VERIFY_SCRIPT,
              SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_P2SH,
              {}, p2wsh, p2wsh_witness);

    // =========================================================================
    // Edge cases
    // =========================================================================
    printf("\nEdge cases:\n");

    // Maximum script size (10000 bytes)
    std::vector<uint8_t> max_script(10000, OP_NOP);
    max_script[9999] = OP_1;  // Must leave true on stack
    WriteSeed(dir, "max_script_size", FUZZ_EVAL_SCRIPT, 0, {}, max_script);

    // Deep nesting (100 levels max)
    std::vector<uint8_t> deep_nest;
    for (int i = 0; i < 100; i++) {
        deep_nest.push_back(OP_1);
        deep_nest.push_back(OP_IF);
    }
    deep_nest.push_back(OP_1);
    for (int i = 0; i < 100; i++) {
        deep_nest.push_back(OP_ENDIF);
    }
    WriteSeed(dir, "deep_nesting", FUZZ_EVAL_SCRIPT, 0, {}, deep_nest);

    // Many stack items
    std::vector<uint8_t> many_items;
    for (int i = 0; i < 200; i++) {
        many_items.push_back(OP_1);
    }
    many_items.push_back(OP_DEPTH);
    WriteSeed(dir, "many_stack_items", FUZZ_EVAL_SCRIPT, 0, {}, many_items);

    // Large push data
    std::vector<uint8_t> large_push;
    std::vector<uint8_t> large_data(520, 0x42);  // Max element size
    PushData(large_push, large_data);
    large_push.push_back(OP_DROP);
    large_push.push_back(OP_1);
    WriteSeed(dir, "large_push", FUZZ_EVAL_SCRIPT, 0, {}, large_push);

    // =========================================================================
    // CHECKSIG mode seeds
    // =========================================================================
    printf("\nCHECKSIG mode:\n");

    // Valid DER signature for CHECKSIG fuzzing
    WriteSeed(dir, "checksig_valid_der", FUZZ_CHECKSIG, SCRIPT_VERIFY_DERSIG,
              dummy_sig, dummy_pubkey);

    // Invalid signature (wrong length)
    std::vector<uint8_t> bad_sig = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x01};
    WriteSeed(dir, "checksig_bad_der", FUZZ_CHECKSIG, SCRIPT_VERIFY_DERSIG,
              bad_sig, dummy_pubkey);

    printf("\nCorpus generation complete!\n");

    return 0;
}
