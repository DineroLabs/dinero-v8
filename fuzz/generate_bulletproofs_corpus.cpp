/**
 * Bulletproofs Seed Corpus Generator
 *
 * Generates valid range proofs to seed the fuzzer.
 * The fuzzer will mutate these to find edge cases.
 *
 * Build:
 *   g++ -O2 -std=c++17 -I../include generate_bulletproofs_corpus.cpp \
 *       -L../build -ldinero_zk -o generate_bulletproofs_corpus
 *
 * Run:
 *   ./generate_bulletproofs_corpus ../build/fuzz_corpus/bulletproofs/
 */

#include "crypto/bulletproofs.h"

#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <random>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

// Generate a seed file with: target_byte + commitment + proof
bool generate_seed(const std::string& dir, const std::string& name,
                   uint8_t target, uint64_t value) {
    // Random blinding factor
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    uint8_t blinding[32];
    for (int i = 0; i < 32; ++i) {
        blinding[i] = dist(gen);
    }

    // Create commitment
    uint8_t commitment[32];
    if (commitment_create(value, blinding, commitment) != 0) {
        std::cerr << "Failed to create commitment for " << name << "\n";
        return false;
    }

    // Generate proof
    uint8_t proof[2048];
    size_t proof_len = 0;
    if (bp_generate(value, blinding, proof, &proof_len) != 0) {
        std::cerr << "Failed to generate proof for " << name << "\n";
        return false;
    }

    // Write seed file: target_byte + commitment + proof
    std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open " << path << "\n";
        return false;
    }

    out.write(reinterpret_cast<char*>(&target), 1);
    out.write(reinterpret_cast<char*>(commitment), 32);
    out.write(reinterpret_cast<char*>(proof), proof_len);
    out.close();

    std::cout << "Generated: " << name << " (" << (1 + 32 + proof_len) << " bytes)\n";
    return true;
}

// Generate commitment arithmetic seed
bool generate_commitment_seed(const std::string& dir, const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    uint8_t blinding1[32], blinding2[32];
    for (int i = 0; i < 32; ++i) {
        blinding1[i] = dist(gen);
        blinding2[i] = dist(gen);
    }

    uint8_t comm1[32], comm2[32];
    commitment_create(1000, blinding1, comm1);
    commitment_create(2000, blinding2, comm2);

    std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    uint8_t target = 3;  // fuzz_commitment_arithmetic
    out.write(reinterpret_cast<char*>(&target), 1);
    out.write(reinterpret_cast<char*>(comm1), 32);
    out.write(reinterpret_cast<char*>(comm2), 32);
    out.close();

    std::cout << "Generated: " << name << " (65 bytes)\n";
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <corpus_dir>\n";
        return 1;
    }

    std::string corpus_dir = argv[1];

    // Initialize bulletproofs
    if (bp_init() != 0) {
        std::cerr << "Failed to initialize bulletproofs\n";
        return 1;
    }

    // Create directory if needed
    fs::create_directories(corpus_dir);

    std::cout << "Generating Bulletproofs seed corpus in " << corpus_dir << "\n";
    std::cout << "=================================================\n\n";

    // Target 0: fuzz_proof_verify - Valid proofs for different values
    generate_seed(corpus_dir, "verify_zero.bin", 0, 0);
    generate_seed(corpus_dir, "verify_one.bin", 0, 1);
    generate_seed(corpus_dir, "verify_small.bin", 0, 1000);
    generate_seed(corpus_dir, "verify_medium.bin", 0, 1000000);
    generate_seed(corpus_dir, "verify_large.bin", 0, 1000000000000ULL);
    generate_seed(corpus_dir, "verify_max.bin", 0, UINT64_MAX);

    // Target 1: fuzz_commitment_create
    generate_seed(corpus_dir, "commit_zero.bin", 1, 0);
    generate_seed(corpus_dir, "commit_max.bin", 1, UINT64_MAX);

    // Target 2: fuzz_proof_generate
    generate_seed(corpus_dir, "generate_small.bin", 2, 100);
    generate_seed(corpus_dir, "generate_large.bin", 2, 1000000000ULL);

    // Target 3: fuzz_commitment_arithmetic
    generate_commitment_seed(corpus_dir, "arithmetic_1.bin");
    generate_commitment_seed(corpus_dir, "arithmetic_2.bin");

    // Target 4: fuzz_batch_verify - Single proof batch
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint8_t> dist(0, 255);

        uint8_t blinding[32];
        for (int i = 0; i < 32; ++i) blinding[i] = dist(gen);

        uint8_t commitment[32];
        commitment_create(5000, blinding, commitment);

        uint8_t proof[2048];
        size_t proof_len = 0;
        bp_generate(5000, blinding, proof, &proof_len);

        std::string path = corpus_dir + "/batch_single.bin";
        std::ofstream out(path, std::ios::binary);
        uint8_t target = 4;
        uint8_t count = 1;
        uint16_t plen = static_cast<uint16_t>(proof_len);
        out.write(reinterpret_cast<char*>(&target), 1);
        out.write(reinterpret_cast<char*>(&count), 1);
        out.write(reinterpret_cast<char*>(commitment), 32);
        out.write(reinterpret_cast<char*>(&plen), 2);
        out.write(reinterpret_cast<char*>(proof), proof_len);
        out.close();
        std::cout << "Generated: batch_single.bin\n";
    }

    // Target 5: fuzz_proof_rewind
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint8_t> dist(0, 255);

        uint8_t blinding[32], nonce[32];
        for (int i = 0; i < 32; ++i) {
            blinding[i] = dist(gen);
            nonce[i] = dist(gen);
        }

        uint8_t commitment[32];
        commitment_create(7500, blinding, commitment);

        uint8_t proof[2048];
        size_t proof_len = 0;
        bp_generate_with_nonce(7500, blinding, nonce, proof, &proof_len);

        std::string path = corpus_dir + "/rewind_valid.bin";
        std::ofstream out(path, std::ios::binary);
        uint8_t target = 5;
        out.write(reinterpret_cast<char*>(&target), 1);
        out.write(reinterpret_cast<char*>(commitment), 32);
        out.write(reinterpret_cast<char*>(nonce), 32);
        out.write(reinterpret_cast<char*>(proof), proof_len);
        out.close();
        std::cout << "Generated: rewind_valid.bin\n";
    }

    std::cout << "\n=================================================\n";
    std::cout << "Seed corpus generation complete!\n";
    std::cout << "Run fuzzer with: ./fuzz_bulletproofs " << corpus_dir << "\n";

    return 0;
}
