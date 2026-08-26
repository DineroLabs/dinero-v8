#include "solo_miner/types.h"
#include "solo_miner/chain_identity.h"
#include "solo_miner/hash_engine.h"
#include "solo_miner/miner.h"
#include "solo_miner/work_template.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <exception>

using namespace dinero::solo;

// Test hex conversion
void testHexConversion() {
    std::cout << "Testing hex conversion... ";

    std::string hex = "0123456789abcdef";
    auto bytes = hexToBytes(hex);
    assert(bytes.size() == 8);
    assert(bytes[0] == 0x01);
    assert(bytes[1] == 0x23);
    assert(bytes[7] == 0xef);

    auto back = bytesToHex(bytes);
    assert(back == hex);

    std::cout << "PASS\n";
}

// Test compact target conversion
void testCompactTarget() {
    std::cout << "Testing compact target conversion... ";

    // Regtest target 0x207fffff
    uint32_t regtest_bits = 0x207fffff;
    Hash256 target = compactToTarget(regtest_bits);

    // Should be a very high target (easy difficulty)
    // 0x207fffff = exponent 0x20 (32), mantissa 0x7fffff
    // Target = 0x7fffff * 256^(32-3) = very large number

    // Just check it's not all zeros
    bool has_nonzero = false;
    for (int i = 0; i < 32; i++) {
        if (target[i] != 0) has_nonzero = true;
    }
    assert(has_nonzero);

    std::cout << "PASS\n";
}

// Test SHA256d
void testSha256d() {
    std::cout << "Testing SHA256d... ";

    // Known test vector: SHA256d("hello")
    // SHA256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
    // SHA256(above)   = 9595c9df90075148eb06860365df33584b75bff782a510c6cd4883a419833d50

    const char* msg = "hello";
    Hash256 hash = HashEngine::sha256d(reinterpret_cast<const uint8_t*>(msg), 5);

    std::string hash_hex = bytesToHex(hash.data(), 32);
    assert(hash_hex == "9595c9df90075148eb06860365df33584b75bff782a510c6cd4883a419833d50");

    std::cout << "PASS\n";
}

// Test hash comparison
void testHashComparison() {
    std::cout << "Testing hash comparison... ";

    Hash256 target{};
    target[0] = 0x00;
    target[1] = 0x00;
    target[2] = 0xFF;
    // Rest are zeros

    // Hash that meets target (lower)
    Hash256 good_hash{};
    good_hash[0] = 0x00;
    good_hash[1] = 0x00;
    good_hash[2] = 0x01;

    // Hash that doesn't meet target (higher)
    Hash256 bad_hash{};
    bad_hash[0] = 0x00;
    bad_hash[1] = 0x01;
    bad_hash[2] = 0x00;

    assert(hashMeetsTarget(good_hash, target) == true);
    assert(hashMeetsTarget(bad_hash, target) == false);

    std::cout << "PASS\n";
}

// Test block header structure size
void testHeaderSize() {
    std::cout << "Testing header size... ";

    static_assert(sizeof(BlockHeader) == 128, "BlockHeader must be 128 bytes");
    static_assert(HEADER_SIZE == 128, "HEADER_SIZE must be 128");

    std::cout << "PASS\n";
}

void testCandidateHeaderCommitsEveryDisplayedField() {
    std::cout << "Testing candidate header field commitment... ";

    WorkTemplate work;
    work.version = 0x01020304;
    work.prev_hash = std::string(64, '1');
    work.merkle_root = std::string(64, '2');
    work.utreexo_root = std::string(64, '3');
    work.timestamp = 0x0102030405060708ULL;
    work.difficulty_bits = 0x1d00cf7e;
    const uint32_t nonce = 0xa1b2c3d4;

    const auto header = work.buildHeader(nonce);
    assert(header.size() == HEADER_SIZE);
    assert(header[0] == 0x04 && header[3] == 0x01);       // version LE
    assert(header[4] == 0x11 && header[35] == 0x11);      // previous hash
    assert(header[36] == 0x22 && header[67] == 0x22);     // Merkle root
    assert(header[68] == 0x33 && header[99] == 0x33);     // Utreexo root
    assert(header[100] == 0x08 && header[107] == 0x01);   // timestamp LE
    assert(header[108] == 0x7e && header[111] == 0x1d);   // nBits LE
    assert(header[112] == 0xd4 && header[115] == 0xa1);   // nonce LE
    for (size_t i = 116; i < HEADER_SIZE; ++i) assert(header[i] == 0);

    auto other = work.buildHeader(nonce + 1);
    assert(HashEngine::hashHeader(header.data()) !=
           HashEngine::hashHeader(other.data()));

    std::cout << "PASS\n";
}

void testChainIdentity() {
    std::cout << "Testing chain identity... ";

    // v7 restart genesis (Apr 17, 2026) — must match dinero/include/consensus/chain_bundle_generated.h
    assert(std::string(kMainnetGenesisHash) ==
           "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f");
    assert(std::string(kTestnetGenesisHash) ==
           "600c229a4335e865267624395c7fafd32453057dfbd5bbb6bc2987bf6c297d38");
    assert(std::string(kRegtestGenesisHash) ==
           "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f");

    std::cout << "PASS\n";
}

void testBackendParsing() {
    std::cout << "Testing backend parsing... ";

    assert(minerBackendFromString("auto") == MinerBackend::Auto);
    assert(minerBackendFromString("CPU") == MinerBackend::Cpu);
    assert(minerBackendFromString("metal") == MinerBackend::Metal);
    assert(minerBackendFromString("cuda") == MinerBackend::Cuda);
    assert(minerBackendFromString("open-cl") == MinerBackend::OpenCl);
    assert(minerBackendToString(MinerBackend::OpenCl) == "opencl");

    bool threw = false;
    try {
        (void)minerBackendFromString("vulkan");
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    std::cout << "PASS\n";
}

int main() {
    std::cout << "\n"
              << "═══════════════════════════════════════════════════════════\n"
              << "  Dinero Solo Miner - Smoke Tests\n"
              << "═══════════════════════════════════════════════════════════\n"
              << std::endl;

    testHexConversion();
    testCompactTarget();
    testSha256d();
    testHashComparison();
    testHeaderSize();
    testCandidateHeaderCommitsEveryDisplayedField();
    testChainIdentity();
    testBackendParsing();

    std::cout << "\n✅ All tests passed!\n" << std::endl;
    return 0;
}
