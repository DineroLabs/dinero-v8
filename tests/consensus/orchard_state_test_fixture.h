#pragma once
#include "orchard_test_fixture.h"
#include "consensus/orchard_state_transition.h"

using StateError = OrchardStateErrorCode;
static_assert(!std::is_default_constructible_v<PreparedOrchardState>);
static_assert(!std::is_copy_assignable_v<PreparedOrchardState>);
static uint256 H(uint8_t n) { uint256 h; h.data[0]=n; return h; }
static VerifiedOrchardAuthorizations Authorized(const std::string& base, bool spending, uint32_t height) {
    Fixture f(base); f.view.height=height;
    f.bundle=Load(base+(spending?"/combined-spend.bundle":"/combined-shield.bundle"));
    f.outputs[0].script_pub_key=f.view.coins.at(Point(f.inputs[0])).scriptPubKey;
    f.outputs[1].script_pub_key=f.view.coins.at(Point(f.inputs[1])).scriptPubKey;
    f.outputs[1].amount_una=spending?56500:51000;
    if (spending) {
        const auto original=f.view.coins; f.view.coins.clear();
        for (size_t i=0;i<2;++i) {
            const auto coin=original.at(Point(f.inputs[i]));
            for(size_t j=0;j<32;++j) f.inputs[i].txid_wire[j]=static_cast<uint8_t>(96+i*32+j);
            f.view.coins.emplace(Point(f.inputs[i]),coin);
        }
    }
    f.Sign(); return VerifyOrchardAuthorizations(f.Snapshot(),f.domain,height+1,{});
}
template<class F> static void StateReject(StateError code,F f) {
    bool rejected=false;
    try { f(); } catch(const OrchardStateError& e) { Require(e.Code()==code);rejected=true; }
    Require(rejected);
}
template<class F> static void LookupReject(Status code,F f) {
    bool rejected=false;
    try { f(); } catch(const OrchardStateLookupError& e) { Require(e.SourceStatus()==code);rejected=true; }
    Require(rejected);
}
