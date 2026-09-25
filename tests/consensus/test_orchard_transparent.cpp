#include "consensus/orchard_transparent.h"
#include "consensus/contextual_locks.h"
#include <openssl/evp.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <source_location>
#include <type_traits>

using namespace dinero;
using namespace dinero::consensus;
using namespace dinero::orchard;
using Bytes = std::vector<uint8_t>;
using Error = OrchardTransparentErrorCode;
static_assert(!std::is_default_constructible_v<VerifiedOrchardTransparentInputs>);
static_assert(!std::is_copy_assignable_v<VerifiedOrchardTransparentInputs>);
static void Require(bool ok, std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("transparent check failed at line " + std::to_string(at.line()));
}
static Bytes Load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("missing fixture " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}
template<class F> static void Reject(Error code, F fn) {
    bool rejected = false;
    try { fn(); } catch (const OrchardTransparentError& e) { Require(e.Code() == code); rejected = true; }
    Require(rejected);
}
static OutPoint Point(const EnvelopeInput& input) {
    uint256 hash; std::copy(input.txid_wire.begin(), input.txid_wire.end(), hash.begin());
    return {TxId(hash), input.output_index};
}
class View final : public ChainStateView {
public:
    std::map<OutPoint, UTXOEntry> coins;
    uint32_t height = 20000;
    StatusOr<UTXOEntry> getCoin(const OutPoint& p) const override {
        auto it = coins.find(p); if (it == coins.end()) return Status::NotFound; return it->second;
    }
    bool hasCoin(const OutPoint& p) const override { return coins.contains(p); }
    uint32_t getHeight() const override { return height; }
};
struct Fixture {
    std::unique_ptr<secp256k1_context, decltype(&secp256k1_context_destroy)> ctx{
        secp256k1_context_create(SECP256K1_CONTEXT_NONE), secp256k1_context_destroy};
    Bytes bundle;
    std::vector<EnvelopeInput> inputs;
    std::vector<TransparentOutput> outputs;
    uint32_t lock = 12345;
    uint64_t fee = 666;
    View view;
    SigningDomain domain{2, {0x6f,0xb7,0x28,0x15,0xae,0x47,0xa0,0x82,
        0xff,0x3b,0x0f,0x45,0x24,0x6c,0x92,0x88,0x88,0xc0,0xd0,0x0e,
        0xf4,0x3f,0x23,0x2c,0x7e,0xf2,0xab,0x36,0x1c,0x00,0x00,0x00}, 0xa1b2c3d4};
    Hash secret1{}, secret2{}; // Public synthetic test keys only, never wallet material.
    Bytes public2;
    explicit Fixture(const std::string& base) : bundle(Load(base + "/candidate-spend.bundle")) {
        const auto tx = TransactionEnvelope::DecodeExact(Load(base + "/candidate-envelope.bin"));
        inputs = tx.Inputs(); outputs = tx.Outputs();
        for (auto& in : inputs) in.witness.clear();
        secret1.back() = 1; secret2.back() = 2;
        secp256k1_keypair pair;
        secp256k1_xonly_pubkey xonly;
        Require(secp256k1_keypair_create(ctx.get(), &pair, secret1.data()));
        Require(secp256k1_keypair_xonly_pub(ctx.get(), &xonly, nullptr, &pair));
        Bytes taproot(34); taproot[0] = 0x51; taproot[1] = 32;
        Require(secp256k1_xonly_pubkey_serialize(ctx.get(), taproot.data() + 2, &xonly));
        secp256k1_pubkey pub;
        Require(secp256k1_ec_pubkey_create(ctx.get(), &pub, secret2.data()));
        public2.resize(33); size_t n = 33;
        Require(secp256k1_ec_pubkey_serialize(ctx.get(), public2.data(), &n, &pub, SECP256K1_EC_COMPRESSED));
        Hash sha; unsigned size = 0;
        Require(EVP_Digest(public2.data(), public2.size(), sha.data(), &size, EVP_sha256(), nullptr) == 1 && size == 32);
        Bytes wpkh(22); wpkh[1] = 20;
        Require(EVP_Digest(sha.data(), sha.size(), wpkh.data() + 2, &size, EVP_ripemd160(), nullptr) == 1 && size == 20);
        view.coins.emplace(Point(inputs[0]), UTXOEntry(AmountUna::Una(12345), taproot, 100, false));
        view.coins.emplace(Point(inputs[1]), UTXOEntry(AmountUna::Una(54321), wpkh, 101, false));
    }
    TransactionEnvelope Build() const { return TransactionEnvelope::Create(lock, inputs, outputs, fee, bundle); }
    OrchardCoinSnapshot Snapshot() const { return OrchardCoinSnapshot::ResolveUnderChainstateLock(Build(), view); }
    void Sign() {
        const auto snapshot = Snapshot();
        for (size_t i = 0; i < inputs.size(); ++i) {
            const auto digest = OrchardTransparentSigningDigest(snapshot, domain, i);
            if (snapshot.Coins()[i].scriptPubKey[0] == 0x51) {
                secp256k1_keypair pair;
                Require(secp256k1_keypair_create(ctx.get(), &pair, secret1.data()));
                Bytes sig(64); Hash aux{};
                Require(secp256k1_schnorrsig_sign32(ctx.get(), sig.data(), digest.data(), &pair, aux.data()));
                inputs[i].witness = {sig};
            } else {
                secp256k1_ecdsa_signature signature;
                Require(secp256k1_ecdsa_sign(ctx.get(), &signature, digest.data(), secret2.data(), nullptr, nullptr));
                Bytes sig(72); size_t n = sig.size();
                Require(secp256k1_ecdsa_signature_serialize_der(ctx.get(), sig.data(), &n, &signature));
                sig.resize(n); sig.push_back(1); inputs[i].witness = {sig, public2};
            }
        }
    }
    VerifiedOrchardTransparentInputs Verify(const OrchardBranchMtpLookup& mtp = {}) const {
        return VerifyOrchardTransparentInputs(Snapshot(), domain, view.height + 1, mtp);
    }
};
static void VectorsAndSignatures(const std::string& base) {
    Fixture f(base);
    const auto unsigned_snapshot = f.Snapshot();
    const auto intent = unsigned_snapshot.SigningDigest(f.domain);
    const auto expected = Load(base + "/candidate-transparent.intent");
    Require(std::equal(intent.begin(), intent.end(), expected.begin(), expected.end()));
    for (size_t i = 0; i < 2; ++i) {
        const auto digest = OrchardTransparentSigningDigest(unsigned_snapshot, f.domain, i);
        const auto bytes = Load(base + (i == 0 ? "/candidate-transparent.taproot" : "/candidate-transparent.p2wpkh"));
        Require(std::equal(digest.begin(), digest.end(), bytes.begin(), bytes.end()));
    }
    Reject(Error::ContextMismatch, [&] { (void)OrchardTransparentSigningDigest(unsigned_snapshot, f.domain, 2); });
    f.Sign(); const auto valid = f.Verify();
    Require(valid.OrchardIntent() == intent && valid.CandidateHeight() == 20001);
    Require(valid.Snapshot().Transaction().CanonicalBytes() == f.Build().CanonicalBytes());
    // These tests authorize transparent inputs only. Changing the saved bundle's
    // original prevout scripts requires new Orchard authorizations for a full tx.
    bool orchard_rejected = false;
    try { (void)valid.Snapshot().VerifyOrchardAuthorization(f.domain); }
    catch (const BackendError&) { orchard_rejected = true; }
    Require(orchard_rejected);
    const auto witnesses = f.inputs;
    f.inputs[0].witness[0][0] ^= 1;
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.inputs[0].witness.push_back({0x50});
    Reject(Error::InvalidWitness, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.inputs[0].witness[0].push_back(0);
    Reject(Error::InvalidWitness, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.inputs[0].witness.clear();
    Reject(Error::InvalidWitness, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.inputs[1].witness.push_back({});
    Reject(Error::InvalidWitness, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.inputs[1].witness[0].back() = 0x81;
    Reject(Error::InvalidWitness, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.inputs[1].witness[0].insert(f.inputs[1].witness[0].end() - 1, 0);
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.inputs[1].witness[1][2] ^= 1;
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.inputs[1].witness[1].resize(65);
    Reject(Error::InvalidWitness, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    // Canonically encoded high-S must reject, not be normalized on admission.
    secp256k1_ecdsa_signature high;
    auto& encoded = f.inputs[1].witness[0];
    Require(secp256k1_ecdsa_signature_parse_der(f.ctx.get(), &high, encoded.data(), encoded.size() - 1));
    std::array<uint8_t, 64> compact;
    Require(secp256k1_ecdsa_signature_serialize_compact(f.ctx.get(), compact.data(), &high));
    constexpr std::array<uint8_t, 32> order = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,
        0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x41};
    int borrow = 0;
    for (int i = 31; i >= 0; --i) {
        const int difference = int(order[i]) - int(compact[32+i]) - borrow;
        compact[32+i] = static_cast<uint8_t>(difference); borrow = difference < 0;
    }
    Require(secp256k1_ecdsa_signature_parse_compact(f.ctx.get(), &high, compact.data()));
    encoded.resize(72); size_t high_size = encoded.size();
    Require(secp256k1_ecdsa_signature_serialize_der(f.ctx.get(), encoded.data(), &high_size, &high));
    encoded.resize(high_size); encoded.push_back(1);
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    const auto domain = f.domain;
    f.domain.network_code = 1;
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.domain = domain;
    f.domain.genesis_wire[0] ^= 1;
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.domain = domain;
    ++f.domain.branch_id;
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.domain = domain;
    ++f.fee; --f.outputs[0].amount_una;
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); --f.fee; ++f.outputs[0].amount_una;
    f.outputs[0].script_pub_key.push_back(0);
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.outputs[0].script_pub_key.pop_back();
    ++f.lock;
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); --f.lock;
    --f.inputs[0].sequence;
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.inputs = witnesses;
    f.bundle.at(54 + 160 + 10) ^= 1; // Public synthetic encrypted payload.
    Require(f.Snapshot().SigningDigest(f.domain) != intent);
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); }); f.bundle.at(54 + 160 + 10) ^= 1;
    auto& a = f.view.coins.at(Point(f.inputs[0])); auto& b = f.view.coins.at(Point(f.inputs[1]));
    a.value = AmountUna::Una(12344); b.value = AmountUna::Una(54322);
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); });
    a.value = AmountUna::Una(12345); b.value = AmountUna::Una(54321);
    // Both inputs now use the SAME key/program; swapping signatures still fails
    // because each signing message commits its input index.
    b.scriptPubKey = a.scriptPubKey; f.Sign(); (void)f.Verify();
    std::swap(f.inputs[0].witness, f.inputs[1].witness);
    Reject(Error::InvalidSignature, [&] { (void)f.Verify(); });
    for (const Bytes script : {Bytes{0x51}, Bytes(34, 0), Bytes{0x52, 0x20}, Bytes{0xa9, 0x14}}) {
        a.scriptPubKey = script;
        Reject(Error::UnsupportedProgram, [&] { (void)f.Verify(); });
    }
}
static void Spendability(const std::string& base) {
    Fixture f(base); f.Sign();
    auto& coin = f.view.coins.at(Point(f.inputs[0]));
    coin.isCoinbase = true; coin.height = 19901; (void)f.Verify();
    coin.height = 19902;
    Reject(Error::ImmatureCoinbase, [&] { (void)f.Verify(); });
    coin.height = UINT32_MAX;
    Reject(Error::ContextMismatch, [&] { (void)f.Verify(); });
    coin.isCoinbase = false; coin.height = 20001; (void)f.Verify();
    Reject(Error::ContextMismatch, [&] { (void)VerifyOrchardTransparentInputs(f.Snapshot(), f.domain, 20002, {}); });
    coin.height = 100; f.lock = 20001; f.Sign();
    Reject(Error::NonFinal, [&] { (void)f.Verify(); });
    f.lock = 20000; f.Sign(); (void)f.Verify();
    f.lock = 600000000; f.Sign();
    Reject(Error::MissingMedianTime, [&] { (void)f.Verify(); });
    Reject(Error::NonFinal, [&] { (void)f.Verify([](auto) { return 600000000ULL; }); });
    (void)f.Verify([](auto) { return 600000001ULL; });
    for (auto& input : f.inputs) input.sequence = UINT32_MAX;
    f.Sign(); (void)f.Verify(); // All-final intentionally disables absolute lock.
    f.lock = 0; f.inputs[0].sequence = 10; coin.height = 19991; f.Sign(); (void)f.Verify();
    coin.height = 19992;
    Reject(Error::NonFinal, [&] { (void)f.Verify(); });
    coin.height = 100; f.inputs[0].sequence = (1U << 22) | 2; f.Sign();
    Reject(Error::MissingMedianTime, [&] { (void)f.Verify(); });
    auto times = [](uint32_t h) -> std::optional<uint64_t> {
        if (h == 99) return 1000; if (h == 20000) return 2024; return std::nullopt;
    };
    (void)f.Verify(times);
    Reject(Error::NonFinal, [&] { (void)f.Verify([](uint32_t h) { return h == 99 ? 1000ULL : 2023ULL; }); });
    Reject(Error::NonFinal, [&] { (void)f.Verify([](uint32_t h) { return h == 99 ? UINT64_MAX : 2024ULL; }); });
    f.inputs[0].sequence = 0x80000000U; f.Sign(); (void)f.Verify();
    // Overflow at the tip cannot wrap candidate height to genesis.
    f.view.height = UINT32_MAX;
    Reject(Error::ContextMismatch, [&] { (void)VerifyOrchardTransparentInputs(f.Snapshot(), f.domain, 0, {}); });
}
static void LockParity(const std::string& base) {
    Fixture f(base);
    unsigned cases = 0;
    for (uint32_t sequence : {0U, 10U, (1U << 22) | 2, 0x80000000U, UINT32_MAX}) {
        for (uint32_t lock : {0U, 20000U, 20001U, 600000000U}) {
            for (bool missing : {false, true}) {
                f.inputs[0].sequence = sequence; f.lock = lock; f.Sign();
                const OrchardBranchMtpLookup lookup = [missing](uint32_t h) -> std::optional<uint64_t> {
                    if (missing) return std::nullopt;
                    return h == 20000 ? 600000001 : 600000000;
                };
                // Comparison of lock rules ONLY. Never use this historical
                // shell for serialization, signing or transaction admission.
                Transaction historical; historical.version = 2; historical.lockTime = lock;
                historical.vin.resize(2);
                historical.vin[0].sequence = sequence; historical.vin[1].sequence = f.inputs[1].sequence;
                std::string reason;
                const bool expected = CheckContextualLocks(historical, 20001, 0, {100,101}, lookup, reason);
                bool accepted = true;
                try { (void)f.Verify(lookup); }
                catch (const OrchardTransparentError& e) {
                    Require(e.Code() == Error::NonFinal || e.Code() == Error::MissingMedianTime); accepted = false;
                }
                Require(accepted == expected); ++cases;
            }
        }
    }
    Require(cases == 40);
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2); VectorsAndSignatures(argv[1]); Spendability(argv[1]); LockParity(argv[1]);
        std::cout << "Orchard transparent authorization: independent digests, real Schnorr/ECDSA, witness restrictions, maturity and contextual locks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
