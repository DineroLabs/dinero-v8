#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_serialization.h"
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace dinero::consensus::shielded;
template <typename F> static double Ms(F&& f) {
    const auto begin = std::chrono::steady_clock::now(); f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count();
}
int main() {
    ShieldedBundle bundle;
    for (size_t i=0; i<kMaxOutputsPerBundle; ++i) {
        ShieldedOutput out{};
        out.commitment[0]=static_cast<uint8_t>(i>>8);
        out.commitment[1]=static_cast<uint8_t>(i);
        out.encrypted_note.assign(611, static_cast<uint8_t>(i));
        out.zk_proof.assign(32, static_cast<uint8_t>(i+1));
        bundle.outputs.push_back(std::move(out));
    }
    const auto wire=SerializeShieldedBundle(bundle); if (wire.empty()) return 2;
    constexpr size_t parses=100;
    const double parse_ms=Ms([&]{ for(size_t i=0;i<parses;++i){ShieldedBundle d;if(DeserializeShieldedBundle(wire,&d)!=BundleDecodeError::Ok)std::abort();}});
    CommitmentTree tree; constexpr size_t leaves=1000;
    const double append_ms=Ms([&]{for(size_t i=0;i<leaves;++i){Hash h{};h[0]=i;h[1]=i>>8;h[2]=i>>16;tree.Append(h);}});
    const double witness_ms=Ms([&]{for(size_t i=0;i<100;++i)if(!tree.GetAuthPath((i*97)%leaves))std::abort();});
    std::cout<<"{\n  \"max_bundle_bytes\": "<<wire.size()<<",\n  \"parse_iterations\": "<<parses
      <<",\n  \"parse_total_ms\": "<<parse_ms<<",\n  \"parse_per_bundle_ms\": "<<parse_ms/parses
      <<",\n  \"tree_leaves\": "<<leaves<<",\n  \"tree_append_total_ms\": "<<append_ms
      <<",\n  \"witness_iterations\": 100,\n  \"witness_total_ms\": "<<witness_ms<<"\n}\n";
}
