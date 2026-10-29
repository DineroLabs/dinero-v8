#pragma once
// Shared public synthetic fixture for component tests only.
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
