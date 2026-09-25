#include "orchard_test_fixture.h"
#include "consensus/orchard_authorization.h"
#include "primitives/transaction_reader.h"

static_assert(!std::is_default_constructible_v<VerifiedOrchardAuthorizations>);
static_assert(!std::is_constructible_v<VerifiedOrchardAuthorizations, VerifiedOrchardTransparentInputs>);
static_assert(!std::is_copy_assignable_v<VerifiedOrchardAuthorizations>);
template<class F> static void BackendReject(int status, F fn) {
    bool rejected = false;
    try { fn(); } catch (const BackendError& e) { Require(e.Status() == status); rejected = true; }
    Require(rejected);
}
static auto VerifyBoth(const Fixture& f) {
    return VerifyOrchardAuthorizations(f.Snapshot(), f.domain, f.view.height + 1, {});
}
static void CombinedCase(const std::string& base, bool spending) {
    Fixture f(base);
    const auto prefix = base + (spending ? "/combined-spend" : "/combined-shield");
    f.bundle = Load(prefix + ".bundle");
    f.outputs[0].script_pub_key = f.view.coins.at(Point(f.inputs[0])).scriptPubKey;
    f.outputs[1].script_pub_key = f.view.coins.at(Point(f.inputs[1])).scriptPubKey;
    f.outputs[1].amount_una = spending ? 56500 : 51000;
    if (spending) {
        // Independent funding coins: do not reuse the shield's inputs in this
        // synthetic spend. These are coin-view fixtures, not mined outputs.
        auto original = f.view.coins;
        f.view.coins.clear();
        for (size_t i = 0; i < 2; ++i) {
            const auto coin = original.at(Point(f.inputs[i]));
            for (size_t j = 0; j < 32; ++j) f.inputs[i].txid_wire[j] = static_cast<uint8_t>(96 + i * 32 + j);
            f.view.coins.emplace(Point(f.inputs[i]), coin);
        }
    }
    f.Sign();
    const auto result = VerifyBoth(f);
    const auto flow = GetOrchardValueFlow(result.Transparent());
    Require(flow.transparent_inputs == 66666 && flow.transparent_outputs == (spending ? 66500 : 61000) && flow.fee == 666);
    const auto pool = ApplyOrchardValueFlows(spending ? 5000 : 0, {flow});
    Require(pool.ok() && pool.value() == (spending ? 4500 : 5000));
    Require(ApplyOrchardValueFlows(spending ? 499 : MAX_MONEY - 4999, {flow}).status() == Status::Invalid);
    const auto expected = Load(prefix + ".digest");
    const auto d = result.Transparent().OrchardIntent();
    Require(std::equal(d.begin(), d.end(), expected.begin(), expected.end()));
    Require(result.Orchard().Orchard().SigningDigest() == d);
    const auto& tx = result.Transaction();
    Require(tx.CanonicalBytes() == f.Build().CanonicalBytes());
    Require(result.Orchard().Txid() == tx.Txid() && result.Orchard().Wtxid() == tx.Wtxid());
    const auto& facts = result.Orchard().Orchard().Facts();
    Require(facts.action_count == 2 && facts.value_balance == (spending ? 500 : -5000));
    const auto effect = Load(prefix + ".effect");
    Require(std::equal(std::begin(facts.effect), std::end(facts.effect), effect.begin(), effect.end()));
    if (spending) {
        const auto anchor = Load(prefix + ".anchor");
        Require(std::equal(std::begin(facts.anchor), std::end(facts.anchor), anchor.begin(), anchor.end()));
        const auto nullifier = Load(prefix + ".note-nullifier");
        bool present = false;
        for (size_t i = 0; i < facts.action_count; ++i)
            present |= std::equal(std::begin(facts.nullifiers[i]), std::end(facts.nullifiers[i]), nullifier.begin(), nullifier.end());
        Require(present);
    }
    // The exact signed outer wire survives typed serialization and re-verifies.
    const auto parsed = ParsedTransaction::DecodeExact(tx.CanonicalBytes(), TransactionReadMode::StagedOrchard);
    const auto roundtrip = VerifyOrchardAuthorizations(
        OrchardCoinSnapshot::ResolveUnderChainstateLock(parsed.Orchard(), f.view), f.domain, 20001, {});
    Require(roundtrip.Transaction().Wtxid() == tx.Wtxid());
    Transaction historical;
    size_t consumed = 0;
    Require(!TransactionSerializer::Deserialize(historical, tx.CanonicalBytes(), consumed));
    Require(consumed == 0);

    const auto signed_inputs = f.inputs;
    f.inputs[0].witness[0][0] ^= 1;
    // Orchard authorization alone still passes; the combined boundary rejects.
    (void)f.Snapshot().VerifyOrchardAuthorization(f.domain);
    Reject(Error::InvalidSignature, [&] { (void)VerifyBoth(f); }); f.inputs = signed_inputs;
    const auto good_bundle = f.bundle;
    f.bundle.at(54 + 884 * 2 + 4 + 30) ^= 1;
    const auto proof_independent_flow = GetOrchardValueFlow(f.Verify());
    Require(proof_independent_flow.transparent_inputs == flow.transparent_inputs &&
            proof_independent_flow.transparent_outputs == flow.transparent_outputs && proof_independent_flow.fee == flow.fee);
    BackendReject(DINERO_ORCHARD_PROOF, [&] { (void)VerifyBoth(f); }); f.bundle = good_bundle;
    f.bundle.at(54 + 820) ^= 1;
    (void)f.Verify();
    BackendReject(DINERO_ORCHARD_SPEND_SIGNATURE, [&] { (void)VerifyBoth(f); }); f.bundle = good_bundle;
    f.bundle.at(f.bundle.size() - 64) ^= 1;
    (void)f.Verify();
    BackendReject(DINERO_ORCHARD_BINDING_SIGNATURE, [&] { (void)VerifyBoth(f); }); f.bundle = good_bundle;
    f.bundle.at(54 + 160 + 10) ^= 1;
    Reject(Error::InvalidSignature, [&] { (void)VerifyBoth(f); });
    f.Sign(); // Even fresh transparent signatures cannot replace Orchard auth.
    BackendReject(DINERO_ORCHARD_SPEND_SIGNATURE, [&] { (void)VerifyBoth(f); });
    f.bundle = good_bundle; f.inputs = signed_inputs;
    auto& coin = f.view.coins.at(Point(f.inputs[0]));
    coin.isCoinbase = true; coin.height = 20000;
    Reject(Error::ImmatureCoinbase, [&] { (void)VerifyBoth(f); });
    coin.isCoinbase = false; coin.height = 100;
    (void)VerifyBoth(f); // A prior negative result does not mutate the fixture.
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2);
        CombinedCase(argv[1], false); CombinedCase(argv[1], true);
        std::cout << "Combined Orchard authorization: valid shielding and cross-address spend/partial withdrawal, both signature paths, proof, identities and rejected isolated failures passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
