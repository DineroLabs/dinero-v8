// Test-only: attach a genuine compact Auth output proof to a supplied fanout
// envelope. Transparent signatures are added by the real daemon wallet RPC.
#include "wallet/shielded_wallet_ops.h"
#include "wallet/shielded_derivation.h"
#include "primitives/transaction.h"
#include <json/json.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: compact_package_builder request.json");
    std::ifstream input(argv[1]); Json::Value request;
    input >> request;
    auto raw = dinero::TransactionSerializer::FromHex(request["hex"].asString());
    dinero::Transaction tx; size_t consumed = 0;
    if (!dinero::TransactionSerializer::Deserialize(tx, raw, consumed) || consumed != raw.size())
        throw std::runtime_error("invalid fixture envelope");
    tx.version = dinero::Transaction::TX_VERSION_SHIELDED_V2;
    tx.witness_version = 0;
    tx.SetExplicitFee(request["fee_una"].asUInt64());
    namespace drv = dinero::wallet::shielded;
    namespace ops = dinero::wallet::shielded_ops;
    std::array<uint8_t, 64> seed{}; seed[0] = 0x91; // public regtest fixture only
    const auto keys = drv::DeriveShieldedAccount(seed.data(), seed.size(), 0);
    const auto address = drv::DeriveDiversifiedAddress(keys, 9, drv::kHrpRegtest);
    ops::AddressedRecipient recipient;
    recipient.d = address.d; recipient.pk_d = address.pk_d;
    recipient.pk_d_spend = address.pk_d_spend; recipient.nfk_commitment = address.nfk_commitment;
    recipient.value_una = request["shield_una"].asUInt64();
    const auto result = ops::BuildAddressedShieldBundleForTx(tx, recipient, nullptr, true, true, nullptr, true);
    if (result.status != ops::OpStatus::Ok) throw std::runtime_error(result.error);
    Json::Value output;
    output["hex"] = tx.SerializeHex(true);
    output["transparent_outputs"] = Json::UInt64(tx.vout.size());
    std::cout << output << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
