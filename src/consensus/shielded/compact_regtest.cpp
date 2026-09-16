#include "consensus/shielded/compact_regtest.h"
#include "consensus/shielded/resource_limits.h"
#include "consensus/shielded/shielded_circuit.h"
#include "../../../contrib/benchmarks/compact_spartan_codec.h"

namespace dinero::consensus::shielded {
namespace {
bool Transform(const ShieldedBundle& source, ShieldedBundle& destination, bool pack) {
    if (!CheckAuthBundleCounts(source.spends.size(), source.outputs.size()) ||
        (source.spends.empty() && source.outputs.empty())) return false;
    ShieldedBundle view = source;
    for (auto& spend : view.spends) {
        const auto circuit = BuildSpendCircuit(SpendWitness{},
            SpendPublicInputs{spend.nullifier, spend.anchor, spend.cv}, true, true);
        const experimental::CompactSpartanCodec codec(6, circuit);
        auto proof = pack ? codec.Pack(spend.zk_proof) : codec.Expand(spend.zk_proof);
        if (!proof) return false;
        spend.zk_proof = std::move(*proof);
    }
    for (auto& output : view.outputs) {
        const auto circuit = BuildOutputCircuit(OutputWitness{},
            OutputPublicInputs{output.commitment, output.cv}, true);
        const experimental::CompactSpartanCodec codec(4, circuit);
        auto proof = pack ? codec.Pack(output.zk_proof) : codec.Expand(output.zk_proof);
        if (!proof) return false;
        output.zk_proof = std::move(*proof);
    }
    destination = std::move(view);
    return true;
}
} // namespace
bool PackCompactRegtestBundle(ShieldedBundle& bundle) {
    return Transform(bundle, bundle, true);
}
bool ExpandCompactRegtestBundle(const ShieldedBundle& bundle, ShieldedBundle& expanded) {
    return Transform(bundle, expanded, false);
}
} // namespace dinero::consensus::shielded
