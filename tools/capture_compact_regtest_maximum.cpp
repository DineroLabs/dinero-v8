// Explicit fixture maintenance only: generates fresh randomized proofs once.
// CI consumes committed bytes and never invokes this producer.
#include "wallet/shielded_wallet_ops.h"
#include "wallet/shielded_derivation.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/shielded_serialization.h"
#include "primitives/transaction.h"
#include <json/json.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace sh = dinero::consensus::shielded;
namespace drv = dinero::wallet::shielded;
namespace ops = dinero::wallet::shielded_ops;
template<class C> std::string Hex(const C& bytes) {
    const char* alphabet = "0123456789abcdef";
    std::string result;
    for (auto b : bytes) { result += alphabet[b >> 4]; result += alphabet[b & 15]; }
    return result;
}
int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: capture_compact_regtest_maximum NEW_DIRECTORY");
    const std::filesystem::path directory(argv[1]);
    if (std::filesystem::exists(directory)) throw std::runtime_error("output directory must be new");
    std::filesystem::create_directories(directory);
    std::array<uint8_t, 64> seed{}; seed[0] = 0x83;
    const auto keys = drv::DeriveShieldedAccount(seed.data(), seed.size(), 0);
    const auto addr = drv::DeriveDiversifiedAddress(keys, 11, drv::kHrpRegtest);
    sh::CommitmentTree tree;
    std::vector<ops::UnshieldNoteInput> notes;
    Json::Value prestate(Json::arrayValue);
    for (uint8_t i = 0; i < 4; ++i) {
        ops::UnshieldNoteInput note;
        note.secret_key = drv::DeriveDiversifiedSpendKey(keys.ask, keys.ak, addr.d).s;
        note.nullifier_key = drv::DeriveDiversifiedNullifierKey(keys.nvk, addr.d);
        std::copy(addr.d.begin(), addr.d.end(), note.d.begin());
        note.randomness[0] = 0x93; note.randomness[31] = i + 1;
        note.value_una = 100'000'000;
        note.key_scheme = dinero::wallet::NoteKeyScheme::Auth;
        sh::Hash value{};
        for (int byte = 0; byte < 8; ++byte) value[31-byte] = (note.value_una >> (8*byte)) & 255;
        const auto cm = sh::NoteCommitment(note.d,
            sh::AuthRecipientCommitmentKey(addr.pk_d_spend, addr.nfk_commitment), value, note.randomness);
        prestate.append(Hex(cm));
        note.leaf_index = tree.Append(cm);
        notes.push_back(note);
    }
    for (auto& note : notes) {
        note.anchor = tree.Root();
        note.merkle_path = tree.GetAuthPath(note.leaf_index)->siblings;
    }
    ops::AddressedRecipient recipient;
    recipient.d = addr.d; recipient.pk_d = addr.pk_d;
    recipient.pk_d_spend = addr.pk_d_spend; recipient.nfk_commitment = addr.nfk_commitment;
    recipient.value_una = 70'000'000;
    auto change = recipient; change.value_una = 329'000'000;
    dinero::Transaction tx;
    tx.version = dinero::Transaction::TX_VERSION_COMPACT_REGTEST;
    tx.witness_version = 0; tx.SetExplicitFee(1'000'000);
    const auto built = ops::BuildAddressedTransferBundleForTx(tx, notes, recipient,
        change.value_una, 1'000'000, nullptr, true, true, &change);
    if (built.status != ops::OpStatus::Ok) throw std::runtime_error(built.error);
    sh::ShieldedBundle bundle;
    if (sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle) != sh::BundleDecodeError::Ok)
        throw std::runtime_error("generated bundle cannot decode");
    sh::NullifierSet nullifiers;
    if (nullifiers.Open(":memory:") != sh::NullifierSet::OpenResult::Ok)
        throw std::runtime_error("cannot open temporary nullifier set");
    const auto ctx = sh::BuildShieldedValidationContext(tx, &nullifiers, &tree,
        127, -1'000'000, 0, nullptr, 0, 0, 2, UINT32_MAX, {true, 124});
    if (sh::ValidateShieldedBundle(bundle, ctx) != sh::ShieldedValidationError::Ok)
        throw std::runtime_error("generated proof failed full validation");
    std::ofstream wire(directory / "maximum.tx.hex"); wire << Hex(tx.Serialize(true)) << '\n';
    std::ofstream context(directory / "maximum-prestate.json"); context << prestate << '\n';
    if (!wire || !context) throw std::runtime_error("fixture write failed");
    std::cout << "Captured verified 4-spend/2-output transaction, " << tx.GetSize() << " bytes\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
