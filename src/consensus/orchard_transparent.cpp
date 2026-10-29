#include "consensus/orchard_transparent.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include <openssl/evp.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <algorithm>
#include <array>
#include <map>
#include <string_view>

namespace dinero::consensus {
namespace {
using Hash = orchard::Hash;
using Error = OrchardTransparentErrorCode;
[[noreturn]] void Fail(Error code) { throw OrchardTransparentError(code); }
enum class Program : uint8_t { P2WPKH = 0, TaprootKey = 1 };
Program InputProgram(std::span<const uint8_t> script) {
    if (script.size() == 22 && script[0] == 0 && script[1] == 20) return Program::P2WPKH;
    if (script.size() == 34 && script[0] == 0x51 && script[1] == 32) return Program::TaprootKey;
    Fail(Error::UnsupportedProgram);
}
Hash Sha256(std::span<const uint8_t> bytes) {
    Hash hash;
    unsigned size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), hash.data(), &size, EVP_sha256(), nullptr) != 1 || size != hash.size())
        throw std::runtime_error("Orchard transparent SHA256 unavailable");
    return hash;
}
std::array<uint8_t, 20> Hash160(std::span<const uint8_t> bytes) {
    const auto sha = Sha256(bytes);
    std::array<uint8_t, 20> result;
    unsigned size = 0;
    if (EVP_Digest(sha.data(), sha.size(), result.data(), &size, EVP_ripemd160(), nullptr) != 1 || size != result.size())
        throw std::runtime_error("Orchard transparent RIPEMD160 unavailable");
    return result;
}
Hash SignatureDigest(const Hash& intent, Program program, uint32_t index) {
    constexpr std::string_view tag = "DIN/orchard-v2/transparent-sighash/v1";
    static const Hash tag_hash = Sha256({reinterpret_cast<const uint8_t*>(tag.data()), tag.size()});
    // BIP340-style tagged hash; this is a Dinero-specific ALL-inputs/outputs
    // message, NOT a historical BIP143/BIP341 transaction hash.
    std::array<uint8_t, 102> bytes{};
    auto next = std::copy(tag_hash.begin(), tag_hash.end(), bytes.begin());
    next = std::copy(tag_hash.begin(), tag_hash.end(), next);
    next = std::copy(intent.begin(), intent.end(), next);
    *next++ = ORCHARD_TRANSPARENT_AUTH_PROFILE;
    *next++ = static_cast<uint8_t>(program);
    for (unsigned i = 0; i < 4; ++i) *next++ = static_cast<uint8_t>(index >> (8 * i));
    return Sha256(bytes);
}
void CheckSpendability(const OrchardCoinSnapshot& snapshot, uint32_t height,
                      const OrchardBranchMtpLookup& branch_mtp) {
    if (static_cast<uint64_t>(snapshot.ViewHeight()) + 1 != height) Fail(Error::ContextMismatch);
    const auto& tx = snapshot.Transaction();
    // Cache branch lookups within this validation pass; the caller must keep
    // both the branch and coin view locked, including same-height reorgs.
    std::map<uint32_t, uint64_t> times;
    auto mtp = [&](uint32_t h) {
        if (auto it = times.find(h); it != times.end()) return it->second;
        const auto value = branch_mtp ? branch_mtp(h) : std::nullopt;
        if (!value) Fail(Error::MissingMedianTime);
        times.emplace(h, *value);
        return *value;
    };
    constexpr uint32_t threshold = 500000000U, disabled = 1U << 31, time_based = 1U << 22;
    const bool all_final = std::all_of(tx.Inputs().begin(), tx.Inputs().end(),
        [](const auto& input) { return input.sequence == UINT32_MAX; });
    if (tx.LockTime() && !all_final) {
        const uint64_t cutoff = tx.LockTime() < threshold ? height : mtp(height - 1);
        if (tx.LockTime() >= cutoff) Fail(Error::NonFinal);
    }
    for (size_t i = 0; i < tx.Inputs().size(); ++i) {
        const auto& coin = snapshot.Coins()[i];
        if (coin.height > height) Fail(Error::ContextMismatch);
        // Same 100-block floor as current full-block and authenticated CSN
        // validation. Subtraction after the height check cannot overflow.
        if (coin.isCoinbase && height - coin.height < UTREEXO_STATELESS_COINBASE_MATURITY)
            Fail(Error::ImmatureCoinbase);
        const auto sequence = tx.Inputs()[i].sequence;
        if (sequence & disabled) continue;
        const uint64_t delay = sequence & 0xffffU;
        if (sequence & time_based) {
            const auto origin = mtp(coin.height == 0 ? 0 : coin.height - 1);
            const auto current = mtp(height - 1);
            if (origin > UINT64_MAX - delay * 512 || current < origin + delay * 512) Fail(Error::NonFinal);
        } else if (static_cast<uint64_t>(height) < static_cast<uint64_t>(coin.height) + delay) {
            Fail(Error::NonFinal);
        }
    }
}
void VerifyInput(const orchard::EnvelopeInput& input, const UTXOEntry& coin,
                 Program program, const Hash& digest) {
    static const bool selftested = [] { secp256k1_selftest(); return true; }();
    (void)selftested;
    const auto* ctx = secp256k1_context_static;
    const auto& witness = input.witness;
    if (!input.script_sig.empty()) Fail(Error::InvalidWitness);
    if (program == Program::TaprootKey) {
        // No annex, script-path/control block, alternate sighash or extra item.
        if (witness.size() != 1 || witness[0].size() != 64) Fail(Error::InvalidWitness);
        secp256k1_xonly_pubkey key;
        if (!secp256k1_xonly_pubkey_parse(ctx, &key, coin.scriptPubKey.data() + 2) ||
            !secp256k1_schnorrsig_verify(ctx, witness[0].data(), digest.data(), digest.size(), &key))
            Fail(Error::InvalidSignature);
        return;
    }
    if (witness.size() != 2 || witness[1].size() != 33 ||
        (witness[1][0] != 2 && witness[1][0] != 3) ||
        witness[0].size() < 9 || witness[0].size() > 73 || witness[0].back() != 1)
        Fail(Error::InvalidWitness);
    const auto key_hash = Hash160(witness[1]);
    if (!std::equal(key_hash.begin(), key_hash.end(), coin.scriptPubKey.begin() + 2)) Fail(Error::InvalidSignature);
    secp256k1_pubkey key;
    secp256k1_ecdsa_signature signature;
    const auto der_size = witness[0].size() - 1;
    if (!secp256k1_ec_pubkey_parse(ctx, &key, witness[1].data(), witness[1].size()) ||
        !secp256k1_ecdsa_signature_parse_der(ctx, &signature, witness[0].data(), der_size))
        Fail(Error::InvalidSignature);
    // Strict canonical DER and low-S: never normalize a received signature.
    std::array<uint8_t, 72> canonical;
    size_t size = canonical.size();
    if (!secp256k1_ecdsa_signature_serialize_der(ctx, canonical.data(), &size, &signature) ||
        size != der_size || !std::equal(canonical.begin(), canonical.begin() + size, witness[0].begin()) ||
        secp256k1_ecdsa_signature_normalize(ctx, nullptr, &signature) != 0 ||
        !secp256k1_ecdsa_verify(ctx, &signature, digest.data(), &key)) Fail(Error::InvalidSignature);
}
} // namespace
orchard::Hash OrchardTransparentSigningDigest(const OrchardCoinSnapshot& snapshot,
    orchard::SigningDomain domain, size_t input_index) {
    if (input_index >= snapshot.Coins().size()) Fail(Error::ContextMismatch);
    const auto program = InputProgram(snapshot.Coins()[input_index].scriptPubKey);
    return SignatureDigest(snapshot.SigningDigest(domain), program, static_cast<uint32_t>(input_index));
}
VerifiedOrchardTransparentInputs VerifyOrchardTransparentInputs(
    const OrchardCoinSnapshot& snapshot, orchard::SigningDomain domain,
    uint32_t candidate_height, const OrchardBranchMtpLookup& branch_mtp) {
    CheckSpendability(snapshot, candidate_height, branch_mtp);
    const auto intent = snapshot.SigningDigest(domain); // Once, not once per input.
    for (size_t i = 0; i < snapshot.Coins().size(); ++i) {
        const auto program = InputProgram(snapshot.Coins()[i].scriptPubKey);
        VerifyInput(snapshot.Transaction().Inputs()[i], snapshot.Coins()[i], program,
                    SignatureDigest(intent, program, static_cast<uint32_t>(i)));
    }
    return VerifiedOrchardTransparentInputs(snapshot, intent, candidate_height);
}
OrchardValueFlow GetOrchardValueFlow(const VerifiedOrchardTransparentInputs& inputs) {
    static_assert(orchard::kMaxMoneyUna == MAX_MONEY);
    OrchardValueFlow flow;
    const auto add = [](uint64_t value, uint64_t& sum) {
        if (value > MAX_MONEY || sum > MAX_MONEY - value)
            throw std::invalid_argument("Orchard transparent flow exceeds money bound");
        sum += value;
    };
    for (const auto& coin : inputs.Snapshot().Coins()) add(coin.value.GetUna(), flow.transparent_inputs);
    for (const auto& output : inputs.Snapshot().Transaction().Outputs()) add(output.amount_una, flow.transparent_outputs);
    flow.fee = inputs.Snapshot().Transaction().ExplicitFee();
    if (flow.fee > MAX_MONEY - flow.transparent_outputs)
        throw std::invalid_argument("Orchard transparent outputs and fee exceed money bound");
    return flow;
}
} // namespace dinero::consensus
