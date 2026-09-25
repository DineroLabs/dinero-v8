#include "orchard_test_fixture.h"

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
