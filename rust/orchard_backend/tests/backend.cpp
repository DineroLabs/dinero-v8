#include "orchard_backend.h"
#include "orchard_transaction.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <type_traits>
#include <vector>
using namespace dinero::orchard;
static_assert(kMaxMoneyUna == DINERO_HOST_MAX_MONEY);
static_assert(!std::is_default_constructible_v<TransactionEnvelope>);
static_assert(!std::is_default_constructible_v<VerifiedEnvelopeAuthorization>);
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
static void EnvelopeTests(const std::string& base) {
    FixtureContext fixture;
    std::vector<EnvelopeInput> inputs;
    std::vector<PreviousOutput> coins;
    for (const auto& in:fixture.inputs) {
        inputs.push_back({in.txid_wire,in.output_index,in.sequence,{0x51},{{0x12,0x34},{}}});
        coins.push_back({in.txid_wire,in.output_index,in.amount_una,in.script_pub_key});
    }
    const auto bundle=Load(base+"/candidate-spend.bundle");
    const auto digest_bytes=Load(base+"/candidate-spend.digest");
    const auto tx=TransactionEnvelope::Create(fixture.lock_time,inputs,fixture.outputs,fixture.fee,bundle);
    const auto digest=tx.SigningDigest(fixture.domain,coins);
    Require(std::equal(digest.begin(),digest.end(),digest_bytes.begin()));
    Require(tx.VerifyAuthorization(fixture.domain,coins).Orchard().SigningDigest()==digest);
    const auto wire=tx.CanonicalBytes();
    Require(wire==Load(base+"/candidate-envelope.bin"));
    const auto expected_txid=Load(base+"/candidate-envelope.txid");
    const auto expected_wtxid=Load(base+"/candidate-envelope.wtxid");
    const auto txid=tx.Txid(), wtxid=tx.Wtxid();
    const auto authorized=tx.VerifyAuthorization(fixture.domain,coins);
    Require(authorized.Txid()==txid && authorized.Wtxid()==wtxid && authorized.CanonicalBytes()==wire);
    Require(std::equal(txid.begin(),txid.end(),expected_txid.begin()));
    Require(std::equal(wtxid.begin(),wtxid.end(),expected_wtxid.begin()));
    auto parsed=TransactionEnvelope::DecodeExact(wire);
    Require(parsed.CanonicalBytes()==wire && parsed.Txid()==txid && parsed.Wtxid()==wtxid);
    Require(parsed.VerifyAuthorization(fixture.domain,coins).Orchard().SigningDigest()==digest);
    auto stream=wire; stream.insert(stream.end(),wire.begin(),wire.end());
    auto [first,used]=TransactionEnvelope::DecodePrefix(stream);
    auto [second,used2]=TransactionEnvelope::DecodePrefix(std::span(stream).subspan(used));
    Require(used==wire.size() && used2==wire.size() && first.Txid()==second.Txid());
    Invalid([&]{ (void)TransactionEnvelope::DecodeExact(stream); });
    for (std::size_t n=0;n<wire.size();++n)
        Invalid([&]{ (void)TransactionEnvelope::DecodeExact(std::span(wire).first(n)); });
    for (std::size_t i=0;i<15;++i) {
        auto bad=wire; bad[i]^=1;
        Invalid([&]{ (void)TransactionEnvelope::DecodeExact(bad); });
    }
    for (std::size_t offset:{std::size_t(15),std::size_t(19),std::size_t(59)}) {
        auto bad=wire; std::fill(bad.begin()+offset,bad.begin()+offset+4,0xff);
        Invalid([&]{ (void)TransactionEnvelope::DecodeExact(bad); });
    }
    auto padded=wire; padded.push_back(0);
    const auto padded_length=static_cast<std::uint32_t>(padded.size()-19);
    for (unsigned i=0;i<4;++i) padded[15+i]=static_cast<std::uint8_t>(padded_length>>(8*i));
    Invalid([&]{ (void)TransactionEnvelope::DecodeExact(padded); });
    auto wrong_fee_marker=wire;
    wrong_fee_marker.at(wire.size()-4-bundle.size()-4-8-1)=0;
    Invalid([&]{ (void)TransactionEnvelope::DecodeExact(wrong_fee_marker); });
    auto wrong_coins=coins; std::swap(wrong_coins[0],wrong_coins[1]);
    Invalid([&]{ (void)tx.VerifyAuthorization(fixture.domain,wrong_coins); });
    wrong_coins=coins; wrong_coins.pop_back();
    Invalid([&]{ (void)tx.VerifyAuthorization(fixture.domain,wrong_coins); });
    wrong_coins=coins; ++wrong_coins[0].amount_una; --wrong_coins[1].amount_una;
    bool rejected=false;
    try { (void)tx.VerifyAuthorization(fixture.domain,wrong_coins); }
    catch(const BackendError& e) { rejected=e.Status()==DINERO_ORCHARD_SPEND_SIGNATURE; }
    Require(rejected);
    // Transparent authorization is deliberately excluded from Orchard D, but
    // remains represented by transaction/witness identity and requires host validation.
    auto changed_inputs=inputs; changed_inputs[0].witness[0][0]^=1;
    auto witness_changed=TransactionEnvelope::Create(fixture.lock_time,changed_inputs,fixture.outputs,fixture.fee,bundle);
    Require(witness_changed.Txid()==txid && witness_changed.Wtxid()!=wtxid);
    Require(witness_changed.SigningDigest(fixture.domain,coins)==digest);
    changed_inputs=inputs; changed_inputs[0].script_sig[0]^=1;
    auto script_changed=TransactionEnvelope::Create(fixture.lock_time,changed_inputs,fixture.outputs,fixture.fee,bundle);
    Require(script_changed.Txid()!=txid && script_changed.Wtxid()!=wtxid);
    Require(script_changed.SigningDigest(fixture.domain,coins)==digest);
    auto proof_changed=bundle; proof_changed.at(54+884*2+4+30)^=1;
    auto proof_tx=TransactionEnvelope::Create(fixture.lock_time,inputs,fixture.outputs,fixture.fee,proof_changed);
    Require(proof_tx.Txid()!=txid && proof_tx.SigningDigest(fixture.domain,coins)==digest);
    rejected=false;
    try { (void)proof_tx.VerifyAuthorization(fixture.domain,coins); }
    catch(const BackendError& e) { rejected=e.Status()==DINERO_ORCHARD_PROOF; }
    Require(rejected);
    // Caller buffers are independent after construction.
    inputs[0].sequence=0; coins[0].amount_una=0;
    Require(tx.CanonicalBytes()==wire && tx.Txid()==txid);
    auto duplicate=tx.Inputs(); duplicate.push_back(duplicate[0]);
    Invalid([&]{ (void)TransactionEnvelope::Create(0,duplicate,{},0,bundle); });
    EnvelopeInput coinbase; coinbase.output_index=0xffffffff;
    Invalid([&]{ (void)TransactionEnvelope::Create(0,{coinbase},{},0,bundle); });
    auto excess=tx.Inputs(); excess[0].witness.resize(101);
    Invalid([&]{ (void)TransactionEnvelope::Create(0,excess,{},0,bundle); });
    excess=tx.Inputs(); excess[0].script_sig.resize(10001);
    Invalid([&]{ (void)TransactionEnvelope::Create(0,excess,{},0,bundle); });
    Invalid([&]{ (void)TransactionEnvelope::Create(0,{},{{kMaxMoneyUna,{}}},1,bundle); });
    // Structural encodings only, not proof validity, for the zero-transparent
    // and one-sided flow shapes. Future builder tests must supply valid proofs.
    for (const auto& [ins,outs] : std::vector<std::pair<std::vector<EnvelopeInput>,std::vector<TransparentOutput>>>{
            {{},{}}, {tx.Inputs(),{}}, {{},tx.Outputs()}}) {
        auto shape=TransactionEnvelope::Create(0,ins,outs,0,bundle);
        Require(TransactionEnvelope::DecodeExact(shape.CanonicalBytes()).CanonicalBytes()==shape.CanonicalBytes());
    }
    std::cout << "Draft envelope framing, independent bytes/identities, stream consumption, bounds and transaction-bound authorization passed\n";
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2);
        Require(dinero_orchard_max_money_v1()==kMaxMoneyUna);
        Require(dinero_orchard_max_actions_v1()==kMaxActionsV1);
        const std::string base = argv[1];
        EnvelopeTests(base);
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
            catch (const BackendError& e) { rejected=e.Status()==DINERO_ORCHARD_SPEND_SIGNATURE; }
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
        catch (const BackendError& e) { rejected=e.Status()==DINERO_ORCHARD_SPEND_SIGNATURE; }
        Require(rejected);
        { FixtureContext f; ++f.fee; rejected=false;
          try { (void)parsed.VerifyAuthorization(f.Build()); }
          catch (const BackendError& e) { rejected=e.Status()==DINERO_ORCHARD_BALANCE_MISMATCH; } Require(rejected); }
        // Each cryptographic stage must be enforced through the C++ path too.
        const auto original_bytes=Load(base+"/candidate-spend.bundle");
        for (const auto& [offset, status] : std::vector<std::pair<std::size_t,int>>{
                {54+820,DINERO_ORCHARD_SPEND_SIGNATURE},
                {original_bytes.size()-1,DINERO_ORCHARD_BINDING_SIGNATURE},
                {54+884*2+4+30,DINERO_ORCHARD_PROOF}}) {
            auto altered=original_bytes;
            altered.at(offset)^=1;
            auto candidate=ParsedBundle::Decode(altered);
            Require(candidate.SigningDigest(context)==digest);
            rejected=false;
            try { (void)candidate.VerifyAuthorization(context); }
            catch (const BackendError& e) { rejected=e.Status()==status; }
            Require(rejected);
        }
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
