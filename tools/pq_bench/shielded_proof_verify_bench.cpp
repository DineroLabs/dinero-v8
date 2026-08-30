#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/shielded_circuit.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

using namespace dinero::consensus::shielded;

namespace {
Hash MakeHash(uint8_t first, uint8_t last = 0xcd) {
    Hash h{};
    h[0] = first;
    h[31] = last;
    return h;
}

Hash ValueAsHash(uint64_t value) {
    Hash h{};
    for (int i = 0; i < 8; ++i) h[31 - i] = static_cast<uint8_t>(value >> (8 * i));
    return h;
}

template <typename F> double MeasureMs(F&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start).count();
}
} // namespace

int main() {
    constexpr size_t kIterations = 200;

    OutputWitness output_witness{};
    output_witness.value = ValueAsHash(100'000'000);
    output_witness.public_key = MakeHash(0x02, 0x10);
    output_witness.randomness = MakeHash(0x03, 0x10);
    OutputPublicInputs output_public{};
    output_public.commitment = NoteCommitment(output_witness.d,
        output_witness.public_key, output_witness.value, output_witness.randomness);
    const auto output_proof = ProveOutput(output_witness, output_public, nullptr);
    if (output_proof.empty() || !VerifyOutput(output_proof, output_public, nullptr)) return 2;

    SpendWitness spend_witness{};
    spend_witness.secret_key = MakeHash(0xa1, 0xf0);
    spend_witness.value = ValueAsHash(123'456'789);
    spend_witness.randomness = MakeHash(0xa4, 0xf0);
    const Hash spend_pk = PoseidonHash2(spend_witness.secret_key, Hash{});
    CommitmentTree tree;
    tree.Append(MakeHash(0x10));
    tree.Append(MakeHash(0x11));
    spend_witness.leaf_index = tree.Append(NoteCommitment(
        spend_witness.d, spend_pk, spend_witness.value, spend_witness.randomness));
    const auto path = tree.GetAuthPath(spend_witness.leaf_index);
    if (!path.has_value()) return 3;
    spend_witness.merkle_path = path->siblings;
    SpendPublicInputs spend_public{};
    spend_public.nullifier = ComputeNullifier(spend_witness.secret_key, spend_witness.leaf_index);
    spend_public.anchor = tree.Root();
    const auto spend_proof = ProveSpend(spend_witness, spend_public, nullptr);
    if (spend_proof.empty() || !VerifySpend(spend_proof, spend_public, nullptr)) return 4;

    const double output_ms = MeasureMs([&] {
        for (size_t i = 0; i < kIterations; ++i)
            if (!VerifyOutput(output_proof, output_public, nullptr)) std::abort();
    });
    const double spend_ms = MeasureMs([&] {
        for (size_t i = 0; i < kIterations; ++i)
            if (!VerifySpend(spend_proof, spend_public, nullptr)) std::abort();
    });

    std::cout << "{\n"
              << "  \"iterations_each\": " << kIterations << ",\n"
              << "  \"output_proof_bytes\": " << output_proof.size() << ",\n"
              << "  \"output_verify_total_ms\": " << output_ms << ",\n"
              << "  \"output_verify_per_proof_ms\": " << output_ms / kIterations << ",\n"
              << "  \"spend_proof_bytes\": " << spend_proof.size() << ",\n"
              << "  \"spend_verify_total_ms\": " << spend_ms << ",\n"
              << "  \"spend_verify_per_proof_ms\": " << spend_ms / kIterations << "\n"
              << "}\n";
}
