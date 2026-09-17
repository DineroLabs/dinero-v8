#include "consensus/shielded/compact_regtest.h"
#include "consensus/shielded/resource_limits.h"
#include "consensus/shielded/shielded_circuit.h"
#include "../../../contrib/benchmarks/compact_spartan_codec.h"

namespace dinero::consensus::shielded {
namespace {
const experimental::CompactSpartanCodec& SpendLayout() {
    // Only immutable dimensions and the circuit-structure hash are retained.
    // Public values are assignments, not coefficients, in this fixed profile.
    // ValidateShieldedBundle still verifies every proof against its actual
    // public inputs and current context; no acceptance verdict is cached.
    static const experimental::CompactSpartanCodec codec(
        6, BuildSpendCircuit(SpendWitness{}, SpendPublicInputs{}, true, true));
    return codec;
}

const experimental::CompactSpartanCodec& OutputLayout() {
    static const experimental::CompactSpartanCodec codec(
        4, BuildOutputCircuit(OutputWitness{}, OutputPublicInputs{}, true));
    return codec;
}

bool Transform(const ShieldedBundle& source, ShieldedBundle& destination, bool pack) {
    if (!CheckAuthBundleCounts(source.spends.size(), source.outputs.size()) ||
        (source.spends.empty() && source.outputs.empty())) return false;
    ShieldedBundle view = source;
    for (auto& spend : view.spends) {
        const auto& codec = SpendLayout();
        auto proof = pack ? codec.Pack(spend.zk_proof) : codec.Expand(spend.zk_proof);
        if (!proof) return false;
        spend.zk_proof = std::move(*proof);
    }
    for (auto& output : view.outputs) {
        const auto& codec = OutputLayout();
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
