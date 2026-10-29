#include "consensus/state_commitment.h"
#include <iostream>
#include <stdexcept>
#include <string>

using namespace dinero;
using namespace dinero::consensus;
using Bytes = std::vector<uint8_t>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

static Bytes Hex(const std::string& value) {
    Bytes result;
    for (size_t i = 0; i < value.size(); i += 2)
        result.push_back(static_cast<uint8_t>(std::stoul(value.substr(i, 2), nullptr, 16)));
    return result;
}
static Transaction Coinbase(const Bytes& script) {
    Transaction tx; TxInput input; input.prevout.vout = 0xffffffff;
    input.scriptSig = {1, 1}; tx.vin.push_back(input);
    tx.vout.emplace_back(AmountUna::Zero(), script); return tx;
}
int main() {
    try {
        // Independent fixed wire vectors, not assembled from implementation constants.
        const auto old_wire = Hex("6a25444e525301000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        const auto new_wire = Hex("6a25444e525302000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        uint256 root; for (size_t i = 0; i < 32; ++i) root.data[i] = uint8_t(i);
        constexpr auto legacy = StateCommitmentEncoding::Legacy;
        constexpr auto orchard = StateCommitmentEncoding::Orchard;
        CHECK(BuildStateCommitmentScript(root) == old_wire);
        CHECK(BuildStateCommitmentScript(root, legacy) == old_wire);
        CHECK(BuildStateCommitmentScript(root, orchard) == new_wire);
        CHECK(ParseStateCommitmentScript(old_wire) == root);
        CHECK(!ParseStateCommitmentScript(new_wire));
        CHECK(ParseStateCommitmentScript(new_wire, orchard) == root);
        CHECK(!ParseStateCommitmentScript(old_wire, orchard));
        const auto coinbase = Coinbase(new_wire);
        CHECK(FindStateCommitment(coinbase).status == StateCommitmentStatus::Malformed);
        CHECK(FindStateCommitment(coinbase, orchard).root == root);
        CHECK(FindStateCommitment(coinbase, orchard).status == StateCommitmentStatus::Ok);
        CHECK(FindStateCommitment(Coinbase(old_wire), orchard).status == StateCommitmentStatus::Malformed);
        for (size_t n = 0; n < new_wire.size(); ++n) {
            const Bytes short_wire(new_wire.begin(), new_wire.begin() + n);
            CHECK(!ParseStateCommitmentScript(short_wire, orchard));
        }
        auto long_wire = new_wire; long_wire.push_back(0);
        CHECK(!ParseStateCommitmentScript(long_wire, orchard));
        for (size_t i = 0; i < 7; ++i) {
            auto changed = new_wire; changed[i] ^= 0x80;
            CHECK(!ParseStateCommitmentScript(changed, orchard));
        }
        for (size_t i = 7; i < new_wire.size(); ++i) {
            auto changed = new_wire; changed[i] ^= 1;
            const auto parsed = ParseStateCommitmentScript(changed, orchard);
            CHECK(parsed.has_value() && *parsed != root);
        }
        auto missing = coinbase; missing.vout.clear();
        CHECK(FindStateCommitment(missing, orchard).status == StateCommitmentStatus::Missing);
        for (const auto& second : {old_wire, new_wire, Bytes(new_wire.begin(), new_wire.begin() + 6)}) {
            auto duplicate = coinbase; duplicate.vout.emplace_back(AmountUna::Zero(), second);
            CHECK(FindStateCommitment(duplicate, orchard).status == StateCommitmentStatus::Duplicate);
            CHECK(FindStateCommitment(duplicate).status == StateCommitmentStatus::Duplicate);
        }
        auto invalid = coinbase; invalid.vin.clear();
        CHECK(FindStateCommitment(invalid, orchard).status == StateCommitmentStatus::Malformed);
        for (int field = 0; field < 5; ++field) {
            auto invalid_output = coinbase; auto& out = invalid_output.vout[0];
            if (field == 0) out.value = AmountUna::Una(1);
            if (field == 1) out.is_confidential = true;
            if (field == 2) out.commitment = {1};
            if (field == 3) out.range_proof = {1};
            if (field == 4) out.nonce = {1};
            CHECK(FindStateCommitment(invalid_output, orchard).status == StateCommitmentStatus::Malformed);
            // This is a new-profile rule, not a retroactive change to v1 lookup.
            out.scriptPubKey = old_wire;
            CHECK(FindStateCommitment(invalid_output).status == StateCommitmentStatus::Ok);
        }
        for (const auto version : {0, 3, 255}) {
            const auto unsupported = static_cast<StateCommitmentEncoding>(version);
            bool rejected = false;
            try { (void)BuildStateCommitmentScript(root, unsupported); }
            catch (const std::invalid_argument&) { rejected = true; }
            CHECK(rejected);
            CHECK(!ParseStateCommitmentScript(new_wire, unsupported));
            CHECK(FindStateCommitment(coinbase, unsupported).status == StateCommitmentStatus::Malformed);
        }
        std::cout << "Versioned DNRS vectors, mutual exclusion, malformed/duplicate encodings and legacy parity passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
