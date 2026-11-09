// W4 wallet-fixture generator (separation gate; spec section 8).
//
//   sep_make_wallet_fixture <outdir>
//
// Built ONLY at the baseline (option DINERO_SEPARATION_FIXTURE_GEN). Writes,
// through the wallet's real public APIs (no hand-written SQL into wallet
// tables), a data directory holding:
//   wallet_registry.db
//   wallets/wallet_sepfixture.db  HD wallet from the public BIP39 test
//                                 mnemonic, 3 receive addresses, 1 watch
//                                 script, 1 DescriptorStore entry; in the
//                                 Orchard (ON) build also the Orchard tables
//                                 and the runtime ordinary-delivery triggers
//   blockchain/utxo               UTXOIndex (user_version=2), 2 UTXOs; in the
//                                 ON build also the runtime delivery triggers
// then reopens it twice the way the checker does and writes expected.json and
// schema.txt from the second reopen. Exits nonzero on any failure.

#include "wallet_fixture_common.h"

#include "consensus/coin_type.h"
#include "crypto/hd_keychain.h"
#include "wallet/bip86_descriptor.h"
#include "wallet/descriptor_checksum.h"

#ifdef SEP_FIXTURE_ORCHARD
#include "consensus/merkle_root.h"
#include "daemon/runtime_block_outbox.h"
#include "primitives/block.h"
#endif

#include <cstdio>
#include <exception>
#include <iostream>

using namespace sepfx;

namespace {

void Exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "?";
        sqlite3_free(err);
        Fail(std::string(sql) + ": " + msg);
    }
}

std::vector<uint8_t> Unhex(const std::string& h) {
    Check(h.size() % 2 == 0, "odd hex");
    std::vector<uint8_t> out;
    for (size_t i = 0; i < h.size(); i += 2) out.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)));
    return out;
}

// Public BIP86 account-0 receive descriptor for the fixture mnemonic.
din::DescriptorRecord FixtureDescriptor() {
    using dinero::crypto::HDKeychain;
    const auto master = HDKeychain::fromMnemonic(kMnemonic, "");
    const auto id = master.getHash160();
    auto account = HDKeychain::getBIP86Account(master, dinero::consensus::DINERO_COIN_TYPE, 0);
    account.private_key.fill(0);
    account.is_private = false;
    din::BIP86DescriptorFactory::BIP86Config cfg;
    cfg.master_fingerprint = Hex(id.data(), 4);
    cfg.account_xpub = account.serialize(true);
    cfg.coin_type = dinero::consensus::DINERO_COIN_TYPE;
    cfg.account = 0;
    din::DescriptorRecord r;
    r.descriptor = din::BIP86DescriptorFactory::createReceiveDescriptor(cfg);
    if (r.descriptor.find('#') == std::string::npos) r.descriptor = din::DescriptorChecksum::AddChecksum(r.descriptor);
    Check(din::DescriptorChecksum::Verify(r.descriptor), "descriptor checksum");
    r.checksum = r.descriptor.substr(r.descriptor.find('#') + 1);
    r.policy = "BIP86";
    r.account = 0;
    r.is_change = false;
    r.is_active = true;
    r.created_at = kDescriptorCreatedAt;
    r.label = "sepfixture";
    return r;
}

#ifdef SEP_FIXTURE_ORCHARD
// Account-0 Orchard snapshot, written by the real storage owner exactly as the
// bound-account path does (orchard_account_delivery.cpp:291 + :582):
// InitializeSchemaUnderTransaction creates orchard_wallet_schema/_snapshots,
// StageReplaceRetaining creates orchard_wallet_retained.
void WriteOrchardAccount(dinero::WalletManager& wm) {
    using namespace dinero::orchard;
    const auto domain = OrchardDomain();
    auto lease = wm.AcquireDatabaseLease();
    const auto wid = WalletIdBytes(lease->EnsureDeliveryIdentity());
    auto seed = lease->CopyRecoverySeed(lease->Session());
    sqlite3* db = lease->Database();
    Exec(db, "BEGIN IMMEDIATE");
    try {
        WalletSnapshotStore::InitializeSchemaUnderTransaction(db);
        WalletSnapshotStore store(db, {WalletNetwork::Regtest, domain.genesis_wire, wid, 0}, seed->Bytes());
        const auto keys = WalletKeys::FromSeed(seed->Bytes(), 0);
        const auto origin = dinero::wallet::OrchardAccountState::Begin(domain, keys.ExportFullViewingKey(),
                                                                       kOrchardActivation, Filled(0x93));
        Check(store.StageReplaceRetaining(0, origin.Encode()) == 1, "Orchard origin snapshot");
        const auto issued = origin.IssueReceiver(WalletScope::External).first;
        Check(store.StageReplaceRetaining(1, issued.Encode()) == 2, "Orchard issued snapshot");
        Exec(db, "COMMIT");
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

// One pre-activation (historical-profile) delivery event: a coinbase-only
// block paying OP_TRUE (not a wallet script) at height kDeliveryHeight. Its
// first delivery is what creates the runtime_delivery_* triggers on
// wallet_utxos (runtime_index_delivery.cpp:345) and the runtime_ordinary_*
// triggers plus wallet_meta columns (runtime_index_delivery.cpp:1463-1467).
dinero::RuntimeOutboxEvent DeliveryEvent() {
    dinero::Block block{};
    dinero::Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout = dinero::TxOutPoint(dinero::TxId(uint256{}), UINT32_MAX);
    coinbase.vin[0].scriptSig = {1, 1};
    coinbase.vout.emplace_back(dinero::AmountUna::Zero(), std::vector<uint8_t>{0x51});
    block.vtx.push_back(coinbase);
    block.header.version = 1;
    block.header.prev_block_hash = Filled(0x93);
    block.header.timestamp = 20000;
    block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);
    dinero::RuntimeOutboxEvent e;
    e.cursor = {1, Filled(0x31)};
    e.direction = dinero::RuntimeBlockDirection::Connect;
    e.context.height = kDeliveryHeight;
    e.context.block_hash = block.GetHash();
    e.context.parent_hash = block.header.prev_block_hash;
    e.context.activation_height = kOrchardActivation;
    e.context.domain = OrchardDomain();
    const auto body = block.Serialize();
    e.body.assign(body.begin(), body.end());
    return e;
}
#endif

void Populate(const fs::path& root, const std::string& schema) {
    dinero::WalletManager wm(root, nullptr, schema);
    wm.createFromBip39(kWalletName, kMnemonic, "");
    wm.open(kWalletName);
    fs::create_directories(UtxoDb(root).parent_path());
    dinero::UTXOIndex index(UtxoDb(root).string());
    Check(index.Initialize(), "UTXOIndex::Initialize failed");
    wm.setUTXOIndex(&index);

    const auto receive_count = [&] {
        int n = 0;
        for (const auto& r : wm.listAddresses(false)) if (!r.external && r.change == 0) ++n;
        return n;
    };
    for (int guard = 0; receive_count() < kReceiveAddresses; ++guard) {
        Check(guard < kReceiveAddresses, "address generation did not converge");
        Check(!wm.getNewAddress("", "taproot").empty(), "getNewAddress failed");
    }
    Check(receive_count() == kReceiveAddresses, "receive address count");

    wm.addWatchScript(WatchScript(), kWatchPath, false);
    Check(wm.getWatchScriptPath(WatchScript()).value_or("") == kWatchPath, "watch script not stored");

    // Two synthetic, non-coinbase UTXOs on the first two receive addresses.
    auto rows = wm.listAddresses(false);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
    const struct { const char* txid; uint32_t vout; uint64_t una; int height; } coins[] = {
        {"1111111111111111111111111111111111111111111111111111111111111111", 0, 150000000, 100},
        {"2222222222222222222222222222222222222222222222222222222222222222", 1, 25000000, 101},
    };
    for (size_t i = 0; i < 2; ++i) {
        const auto& r = rows.at(i);
        const auto path = wm.getDerivationPath(r.script_pubkey);
        Check(path.has_value(), "address has no derivation path");
        uint256 txid;
        Check(uint256::FromHex(coins[i].txid, txid), "txid hex");
        Check(index.AddUTXO(dinero::WalletUTXO(dinero::TxId(txid), coins[i].vout,
                                               dinero::AmountUna::Una(coins[i].una), Unhex(r.script_pubkey), *path,
                                               coins[i].height, false)),
              "UTXOIndex::AddUTXO failed");
        Check(wm.addUTXO(coins[i].txid, int(coins[i].vout), int64_t(coins[i].una), r.address, r.script_pubkey,
                         coins[i].height, false),
              "WalletManager::addUTXO failed");
    }
    wm.setBlockchainHeight(kTipHeight);

#ifdef SEP_FIXTURE_ORCHARD
    WriteOrchardAccount(wm);
    const auto session = wm.AcquireDatabaseLease()->Session();
    const auto event = DeliveryEvent();
    (void)dinero::RuntimeIndexDelivery::ApplyForWallet(wm, index, session, event);
    (void)dinero::RuntimeOrdinaryDelivery::ApplyForWallet(wm, session, event);
#endif
    wm.setUTXOIndex(nullptr);
}

void AddDescriptor(const fs::path& root) {
    din::DescriptorStore store(WalletDb(root).string());
    Check(store.initialize(), "DescriptorStore::initialize failed");
    Check(store.addDescriptor(FixtureDescriptor()), "DescriptorStore::addDescriptor failed");
    store.shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
            return 2;
        }
        // Used as given: the registry records the wallet path under this
        // root, so run with a relative <outdir> to keep machine paths out of
        // the committed registry (open() repairs the path on relocation).
        const fs::path root = argv[1];
        Check(!fs::exists(root) || fs::is_empty(root), "outdir must be absent or empty: " + root.string());
        fs::create_directories(root);
        dinero::SelectParams(dinero::Chain::REGTEST);
        const std::string schema = SEP_WALLET_SCHEMA_PATH;
        Check(fs::is_regular_file(schema), "wallet schema missing: " + schema);

        Populate(root, schema);
        AddDescriptor(root);
        RequireNoSidecars(root);

        // Reopen twice: the committed files are the fixed point of an open.
        const std::string tree = "d blockchain\nd wallets\nf blockchain/utxo\nf wallet_registry.db\nf wallets/wallet_sepfixture.db\n";
        const auto first = Canonical(Derive(root, schema));
        const auto schema_first = SchemaDump(root);
        const auto rows_first = RowDump(root);
        const auto header_first = HeaderDump(root);
        Check(TreeDump(root) == tree, "data directory holds files other than the three databases");
        const auto second = Canonical(Derive(root, schema));
        Check(first == second, "derived contents change across reopen (open is not a fixed point)");
        Check(schema_first == SchemaDump(root), "schema changes across reopen (open is not a fixed point)");
        Check(rows_first == RowDump(root), "persisted rows outside ExcludedColumns() change across reopen");
        Check(header_first == HeaderDump(root), "database header fields change across reopen");
        Check(TreeDump(root) == tree, "reopen adds or removes files in the data directory");
        Check(RegistryWalletPath(root) == ExpectedRegistryWalletPath(root),
              "registry wallet path is not <outdir>/wallets/wallet_sepfixture.db");
        const auto schema_second = SchemaDump(root);

        WriteFile(root / "expected.json", second);
        WriteFile(root / "schema.txt", schema_second);
        std::cout << "W4 fixture written: " << root << " (config " << kConfig << ")\n";
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "W4 GENERATOR FAIL: %s\n", e.what());
        return 1;
    }
}
