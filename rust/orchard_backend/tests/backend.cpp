#include "orchard_backend.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <type_traits>
#include <vector>
using namespace dinero::orchard;
static_assert(!std::is_default_constructible_v<ParsedBundle>);
static_assert(!std::is_default_constructible_v<VerifiedAuthorization>);
static_assert(!std::is_default_constructible_v<SigningContext>);
template<class T> concept AcceptsRawDigest = requires(T t, Hash h) { t.VerifyAuthorization(h, 0); };
static_assert(!AcceptsRawDigest<ParsedBundle>);
static_assert(!std::is_assignable_v<decltype(std::declval<ParsedBundle>().UnverifiedFacts()),
                                   DineroOrchardFacts>);
static void Require(bool ok, std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("test check failed at line " + std::to_string(at.line()));
}
static auto Load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("missing fixture: " + path);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}
struct FixtureContext {
    SigningDomain domain{2, {0x6f,0xb7,0x28,0x15,0xae,0x47,0xa0,0x82,
        0xff,0x3b,0x0f,0x45,0x24,0x6c,0x92,0x88,0x88,0xc0,0xd0,0x0e,
        0xf4,0x3f,0x23,0x2c,0x7e,0xf2,0xab,0x36,0x1c,0x00,0x00,0x00}, 0xa1b2c3d4};
    std::uint32_t lock_time = 12345;
    std::vector<ResolvedInput> inputs;
    std::vector<TransparentOutput> outputs{{10000,{0x76,0xa9,0x14,0x11}},
                                         {56000,{0x51,0x20,0x22,0x33}}};
    std::uint64_t fee = 666;
    FixtureContext() {
        ResolvedInput a{}, b{};
        for (unsigned i=0; i<32; ++i) { a.txid_wire[i]=32+i; b.txid_wire[i]=64+i; }
        a.output_index=3; a.sequence=0xfffffffd; a.amount_una=12345; a.script_pub_key={0x51,0xac};
        b.output_index=9; b.sequence=0xfffffffe; b.amount_una=54321; b.script_pub_key={0x00,0x14,0x77};
        inputs={a,b};
    }
    SigningContext Build() const { return SigningContext::Create(domain,lock_time,inputs,outputs,fee); }
};
template<class F> static void Invalid(F f) {
    bool rejected=false;
    try { f(); } catch (const std::invalid_argument&) { rejected=true; }
    Require(rejected);
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2);
        const std::string base = argv[1];
        auto bytes = Load(base + "/candidate-spend.bundle");
        const auto digest_bytes = Load(base + "/candidate-spend.digest");
        Require(digest_bytes.size() == 32);
        Hash digest{};
        std::copy(digest_bytes.begin(), digest_bytes.end(), digest.begin());
        auto parsed = ParsedBundle::Decode(bytes);
        FixtureContext source;
        const auto context=source.Build();
        Require(context.RequiredValueBalance()==0);
        Require(parsed.UnverifiedFacts().action_count==2);
        // Compare with the separately produced signing-preimage fixture.
        Require(parsed.SigningDigest(context)==digest);
        // Caller mutation cannot change either owned snapshot.
        source.inputs[0].amount_una=0; source.outputs.clear(); source.domain.branch_id=0;
        std::fill(bytes.begin(), bytes.end(), 0);
        auto verified = [&] { auto temporary=parsed; return temporary.VerifyAuthorization(context); }();
        Require(verified.SigningDigest()==digest);
        Require(verified.Facts().action_count==2);
        unsigned mutation_checks=0;
        const auto reject_context=[&](const FixtureContext& f) {
            const auto changed=f.Build();
            Require(parsed.SigningDigest(changed)!=digest);
            bool rejected=false;
            try { (void)parsed.VerifyAuthorization(changed); }
            catch (const BackendError& e) { rejected=e.Status()==7; }
            Require(rejected); ++mutation_checks;
        };
        { FixtureContext f; f.domain.network_code=1; reject_context(f); }
        { FixtureContext f; f.domain.genesis_wire[0]^=1; reject_context(f); }
        { FixtureContext f; ++f.domain.branch_id; reject_context(f); }
        { FixtureContext f; ++f.lock_time; reject_context(f); }
        { FixtureContext f; f.inputs[0].txid_wire[0]^=1; reject_context(f); }
        { FixtureContext f; ++f.inputs[0].output_index; reject_context(f); }
        { FixtureContext f; --f.inputs[0].sequence; reject_context(f); }
        { FixtureContext f; ++f.inputs[0].amount_una; --f.inputs[1].amount_una; reject_context(f); }
        { FixtureContext f; f.inputs[0].script_pub_key[0]^=1; reject_context(f); }
        { FixtureContext f; f.outputs[0].script_pub_key[0]^=1; reject_context(f); }
        { FixtureContext f; ++f.fee; --f.outputs[0].amount_una; reject_context(f); }
        { FixtureContext f; std::swap(f.inputs[0],f.inputs[1]); reject_context(f); }
        { FixtureContext f; std::swap(f.outputs[0],f.outputs[1]); reject_context(f); }
        // A changed synthetic encrypted payload must derive a different digest
        // from its own parsed effect and fail authorization. No stale D is supplied.
        auto changed_bytes=Load(base+"/candidate-spend.bundle");
        changed_bytes.at(54+160+10)^=1;
        auto changed_bundle=ParsedBundle::Decode(changed_bytes);
        Require(changed_bundle.SigningDigest(context)!=digest);
        bool rejected=false;
        try { (void)changed_bundle.VerifyAuthorization(context); }
        catch (const BackendError& e) { rejected=e.Status()==7; }
        Require(rejected);
        { FixtureContext f; ++f.fee; rejected=false;
          try { (void)parsed.VerifyAuthorization(f.Build()); }
          catch (const BackendError& e) { rejected=e.Status()==13; } Require(rejected); }
        ResolvedInput ten{}; ten.amount_una=10;
        Require(SigningContext::Create({},0,{ten},{},0).RequiredValueBalance()==-10);
        Require(SigningContext::Create({},0,{},{{10,{}}},0).RequiredValueBalance()==10);
        Invalid([&]{ (void)SigningContext::Create({},0,{ten,ten},{},0); });
        Invalid([&]{ (void)SigningContext::Create({},0,{},{{kMaxMoneyUna,{}}},1); });
        Invalid([&]{ (void)SigningContext::Create({},0,{}, {},kMaxMoneyUna+1); });
        Invalid([&]{ (void)SigningContext::Create({3,{},0},0,{}, {},0); });
        Invalid([&]{ (void)SigningContext::Create({},0,{},{{0,std::vector<std::uint8_t>(10001)}},0); });
        Invalid([&]{ (void)SigningContext::Create({},0,{},std::vector<TransparentOutput>(4097),0); });
        Invalid([&]{ (void)SigningContext::Create({},0,{},
            std::vector<TransparentOutput>(105,{0,std::vector<std::uint8_t>(10000)}),0); });
        rejected=false;
        try { (void)ParsedBundle::Decode(bytes); } catch (const BackendError&) { rejected=true; }
        Require(rejected);
        std::cout << "Orchard C++ ownership, independent digest, " << mutation_checks
                  << " context mutations, payload binding and monetary bounds passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
