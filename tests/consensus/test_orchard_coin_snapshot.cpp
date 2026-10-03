#include "consensus/orchard_coin_snapshot.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <source_location>
#include <type_traits>

using namespace dinero;
using namespace dinero::consensus;
using namespace dinero::orchard;
using Bytes = std::vector<uint8_t>;
static_assert(!std::is_default_constructible_v<OrchardCoinSnapshot>);
static_assert(!std::is_copy_assignable_v<OrchardCoinSnapshot>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<OrchardCoinSnapshot>().Coins())>>);
static void Require(bool ok, std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("coin snapshot check failed at line " + std::to_string(at.line()));
}
static Bytes Load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("missing fixture " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}
template<class F> static void Reject(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    Require(rejected);
}
static OutPoint Point(const EnvelopeInput& input) {
    uint256 hash;
    std::copy(input.txid_wire.begin(), input.txid_wire.end(), hash.begin());
    return {TxId(hash), input.output_index};
}
// Synthetic coin-view fixture. This does not claim an authenticated mainnet view
// or valid transparent signatures; the saved Orchard proof is genuinely checked.
class View final : public ChainStateView {
public:
    std::map<OutPoint, UTXOEntry> coins;
    std::vector<OutPoint> mutable reads;
    uint32_t height = 200;
    mutable unsigned height_reads = 0;
    bool drift = false;
    Status failure = Status::Ok;
    StatusOr<UTXOEntry> getCoin(const OutPoint& point) const override {
        reads.push_back(point);
        if (failure != Status::Ok) return failure;
        auto it = coins.find(point);
        if (it == coins.end()) return Status::NotFound;
        return it->second;
    }
    bool hasCoin(const OutPoint&) const override { throw std::runtime_error("unexpected hasCoin probe"); }
    uint32_t getHeight() const override { return height + (drift && height_reads++ > 0 ? 1 : 0); }
};
static void LookupFailure(const TransactionEnvelope& tx, View& view, Status expected) {
    bool rejected = false;
    try { (void)OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view); }
    catch (const OrchardCoinLookupError& e) { Require(e.SourceStatus() == expected); rejected = true; }
    Require(rejected);
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2);
        const auto base = std::string(argv[1]);
        const auto tx = TransactionEnvelope::DecodeExact(Load(base + "/candidate-envelope.bin"));
        Require(tx.Inputs().size() == 2);
        const SigningDomain domain{2, {0x6f,0xb7,0x28,0x15,0xae,0x47,0xa0,0x82,
            0xff,0x3b,0x0f,0x45,0x24,0x6c,0x92,0x88,0x88,0xc0,0xd0,0x0e,
            0xf4,0x3f,0x23,0x2c,0x7e,0xf2,0xab,0x36,0x1c,0x00,0x00,0x00}, 0xa1b2c3d4};
        View view;
        const auto a = Point(tx.Inputs()[0]), b = Point(tx.Inputs()[1]);
        view.coins.emplace(b, UTXOEntry(AmountUna::Una(54321), {0x00,0x14,0x77}, 7, true));
        view.coins.emplace(a, UTXOEntry(AmountUna::Una(12345), {0x51,0xac}, 9, false));
        // Pin wire byte order and index independently of the resolver.
        Require(a.txid.AsUint256().begin()[0] == 32 && a.txid.AsUint256().begin()[31] == 63 && a.vout == 3);
        const auto original = view.coins;
        const auto snapshot = OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view);
        Require(view.reads == std::vector<OutPoint>{a,b});
        Require(snapshot.ViewHeight() == 200 && snapshot.Coins().size() == 2);
        Require(snapshot.Coins()[0].height == 9 && !snapshot.Coins()[0].isCoinbase);
        Require(snapshot.Coins()[1].height == 7 && snapshot.Coins()[1].isCoinbase);
        const auto digest = snapshot.SigningDigest(domain);
        const auto expected = Load(base + "/candidate-spend.digest");
        Require(std::equal(digest.begin(), digest.end(), expected.begin(), expected.end()));
        const auto verified = snapshot.VerifyOrchardAuthorization(domain);
        Require(verified.Orchard().SigningDigest() == digest);
        Require(verified.CanonicalBytes() == tx.CanonicalBytes());
        // No references into a mutable view survive resolution.
        view.coins[a].scriptPubKey.push_back(0x51);
        const auto changed = OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view);
        Require(changed.SigningDigest(domain) != digest);
        Reject([&] { (void)changed.VerifyOrchardAuthorization(domain); });
        view.coins.clear();
        Require(snapshot.SigningDigest(domain) == digest && snapshot.Coins()[0].scriptPubKey == Bytes({0x51,0xac}));
        LookupFailure(tx, view, Status::NotFound);
        view.coins = original;
        view.coins.erase(b); // Partial resolution cannot publish a snapshot.
        LookupFailure(tx, view, Status::NotFound);
        view.coins = original;
        for (Status failure : {Status::Io, Status::Corruption, Status::Internal}) {
            view.failure = failure; LookupFailure(tx, view, failure);
        }
        view.failure = Status::Ok;
        view.drift = true; view.height_reads = 0;
        LookupFailure(tx, view, Status::Internal);
        view.drift = false;
        view.coins[a].is_confidential = true;
        Reject([&] { (void)OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view); });
        view.coins = original; view.coins[a].commitment = {1};
        Reject([&] { (void)OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view); });
        view.coins = original; view.coins[a].value = AmountUna::UnsafeFromRaw(kMaxMoneyUna + 1);
        Reject([&] { (void)OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view); });
        view.coins[a].value = AmountUna::Max();
        Reject([&] { (void)OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view); });
        view.coins = original;
        view.coins[a].value = AmountUna::Una(12344);
        const auto wrong_balance = OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view);
        Reject([&] { (void)wrong_balance.SigningDigest(domain); });
        Reject([&] { (void)wrong_balance.VerifyOrchardAuthorization(domain); });
        // Preserve the aggregate to require per-input signing, rather than
        // passing only because the independent balance check rejected first.
        view.coins[b].value = AmountUna::Una(54322);
        const auto changed_amount = OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view);
        Require(changed_amount.SigningDigest(domain) != digest);
        Reject([&] { (void)changed_amount.VerifyOrchardAuthorization(domain); });
        // Resolution is not admission: immature metadata remains visible for the
        // subsequent spendability gate, and does not get dropped or rewritten.
        view.coins = original; view.coins[b].height = 200;
        const auto immature = OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, view);
        Require(immature.Coins()[1].height == 200 && immature.Coins()[1].isCoinbase);
        const auto no_inputs = TransactionEnvelope::Create(0, {}, {}, 0, Load(base + "/candidate-spend.bundle"));
        view.reads.clear();
        const auto empty = OrchardCoinSnapshot::ResolveUnderChainstateLock(no_inputs, view);
        Require(empty.Coins().empty() && view.reads.empty());
        std::cout << "Orchard coin resolution: ordered lookups, owned metadata, signing binding, lookup failures and money bounds passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
