// Experimental codec only: no daemon parser or new consensus acceptance path.
#include "consensus/shielded/compact_spartan_codec.h"
#include "zk/zkvm/r1cs_spartan.h"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

namespace {
using namespace dinero::zk::zkvm;
using dinero::consensus::shielded::CompactSpartanCodec;

struct Fixture {
    R1CS circuit;
    std::unique_ptr<CompactSpartanCodec> codec;
    std::vector<uint8_t> full, compact;
    explicit Fixture(uint8_t profile) {
        const std::unique_ptr<secp256k1_context, decltype(&secp256k1_context_destroy)> ctx(
            secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY),
            secp256k1_context_destroy);
        auto x = circuit.alloc(Scalar(1));
        for (int i = 0; i < 5; ++i)
            circuit.constrain(LinearCombination(x), LinearCombination(x), LinearCombination(x));
        Transcript tp("compact.prototype.fuzz");
        const auto &gens = GeneratorSet::cached(8, ctx.get());
        const auto proof = r1cs_spartan_prove(circuit, std::vector<Scalar>(5, Scalar::zero()),
                                              Scalar::one(), gens, tp, ctx.get());
        Transcript tv("compact.prototype.fuzz");
        if (!r1cs_spartan_verify(proof, circuit, circuit.num_constraints(), circuit.num_variables(),
                                 spartan_hash_r1cs_structure(circuit), Scalar::one(), gens, tv,
                                 ctx.get()))
            std::abort();
        full = proof.serialize(ctx.get());
        full.insert(full.begin(), profile);
        codec = std::make_unique<CompactSpartanCodec>(profile, circuit);
        const auto packed = codec->Pack(full);
        if (!packed)
            std::abort();
        compact = *packed;
    }
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > 65536)
        return 0;
    static const Fixture spend(6), output(4);
    const auto &fixture = data[0] & 4 ? output : spend;
    const bool expand = data[0] & 1;
    const bool structured = data[0] & 2;
    const auto input = std::span(data + 1, size - 1);
    std::vector<uint8_t> bytes;
    if (structured) {
        // Reach all inner fields without requiring the fuzzer to rediscover
        // exact length, circuit hash and every count. Raw mode covers lengths.
        bytes = expand ? fixture.compact : fixture.full;
        for (size_t i = 0; i + 2 < input.size(); i += 3) {
            const size_t offset = (size_t(input[i]) * 256 + input[i + 1]) % bytes.size();
            bytes[offset] ^= input[i + 2];
        }
    } else
        bytes.assign(input.begin(), input.end());
    const auto before = bytes;
    const auto transformed = expand ? fixture.codec->Expand(bytes) : fixture.codec->Pack(bytes);
    if (bytes != before)
        std::abort();
    if (transformed) {
        const auto restored =
            expand ? fixture.codec->Pack(*transformed) : fixture.codec->Expand(*transformed);
        if (!restored || *restored != bytes)
            std::abort();
    }
    // Acceptance is structural, never proof validity. Arbitrary raw input is
    // not passed to the historical allocation-sensitive proof deserializer.
    return 0;
}
