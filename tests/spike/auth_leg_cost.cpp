// tests/spike/auth_leg_cost.cpp
// THROWAWAY spike code — constraint-count profile of today's spend circuit per
// profile (legacy / cv-bound / auth), to size the v2 bundle statement for notes
// created after mainnet height 110000 (auth profile: pk_d = s·G in-circuit).
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_circuit.h"
#include "zk/zkvm/r1cs.h"

#include <cstdio>
#include <cstdlib>

using namespace dinero::consensus::shielded;

static Hash MakeHash(uint8_t seed, uint8_t tail = 0xCD) { Hash h{}; h[0] = seed; h[31] = tail; return h; }
static Hash ValueAsHash(uint64_t v) { Hash h{}; for (int i = 0; i < 8; ++i) h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF); return h; }
static Hash MakeBlind(uint8_t seed) { Hash h{}; h[31] = seed; h[30] = 0x11; h[20] = 0x22; return h; }

int main() {
    if (!PedersenGeneratorsReady()) { std::fprintf(stderr, "Pedersen generators not ready\n"); return 2; }
    const uint64_t value = 100'000'000;
    SpendWitness w{};
    w.secret_key = MakeHash(0x01, 0x10);
    w.nullifier_key = MakeHash(0x02, 0x10);
    w.leaf_index = 2;
    w.value = ValueAsHash(value);
    w.randomness = MakeHash(0x03, 0x10);
    w.d = MakeHash(0x05, 0x10);
    w.rcv = MakeBlind(0x0B);
    for (auto& s : w.merkle_path) s = MakeHash(0x20);
    SpendPublicInputs pub{};
    pub.nullifier = ComputeNullifier(w.secret_key, w.leaf_index);
    pub.anchor = MakeHash(0x30);
    if (PedersenCommit(w.rcv, value, pub.cv) != PedersenResult::Ok) { std::fprintf(stderr, "commit failed\n"); return 2; }

    struct Row { const char* label; bool cv; bool auth; };
    const Row rows[] = {{"legacy", false, false}, {"cv_bound", true, false}, {"auth", true, true}};
    std::printf("{\"rows\":[");
    bool first = true;
    for (const auto& r : rows) {
        auto cs = BuildSpendCircuit(w, pub, r.cv, r.auth, false);
        std::printf("%s{\"label\":\"%s\",\"constraints\":%zu,\"variables\":%zu,\"inputs\":%zu}",
                    first ? "" : ",", r.label, cs.num_constraints(), cs.num_variables(), cs.num_inputs());
        first = false;
    }
    std::printf("]}\n");
    return 0;
}
