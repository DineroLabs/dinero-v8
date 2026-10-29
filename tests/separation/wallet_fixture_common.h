// W4 wallet-file compatibility fixtures (separation gate; spec section 8).
//
// Shared by the generator (make_wallet_fixture.cpp, built only at the
// baseline) and the checker (wallet_fixture_compat.cpp, built by every later
// stage). Both derive expected.json with this one function, so a difference
// can only come from the wallet code under test, never from two derivations.
//
// Every failure throws; nothing here relies on assert() (NDEBUG builds).
#pragma once

#include "consensus/chainparams.h"
#include "sqlite_open.h"
#include "wallet/descriptor_store.h"
#include "wallet/utxo_index.h"
#include "wallet/wallet_manager.h"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef SEP_FIXTURE_ORCHARD
#include "orchard_wallet.h"
#include "orchard_wallet_storage.h"
#include "wallet/orchard_account_state.h"
#include "wallet/runtime_index_delivery.h"
#endif

namespace sepfx {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;
using dinero::uint256;

// Public BIP39 test vector mnemonic. No real keys.
inline constexpr const char* kMnemonic =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
inline constexpr const char* kWalletName = "sepfixture";
inline constexpr int kReceiveAddresses = 3;
// Fixed synthetic watch script (P2TR shape, x-only key 0x5e..5e) and path.
inline std::vector<uint8_t> WatchScript() {
    std::vector<uint8_t> s{0x51, 0x20};
    s.insert(s.end(), 32, 0x5e);
    return s;
}
inline constexpr const char* kWatchPath = "m/86'/1448'/0'/0/1000";
inline constexpr int64_t kDescriptorCreatedAt = 1700000000;
inline constexpr uint32_t kTipHeight = 110;

#ifdef SEP_FIXTURE_ORCHARD
inline constexpr const char* kConfig = "on";
#else
inline constexpr const char* kConfig = "off";
#endif

// The three wallet databases, relative to the data directory.
inline const std::vector<std::string>& DatabaseFiles() {
    static const std::vector<std::string> files{
        "wallet_registry.db", "wallets/wallet_sepfixture.db", "blockchain/utxo"};
    return files;
}
inline fs::path WalletDb(const fs::path& root) { return root / "wallets" / "wallet_sepfixture.db"; }
inline fs::path UtxoDb(const fs::path& root) { return root / "blockchain" / "utxo"; }

[[noreturn]] inline void Fail(const std::string& what) { throw std::runtime_error(what); }
inline void Check(bool ok, const std::string& what) { if (!ok) Fail(what); }

inline std::string Hex(const uint8_t* p, size_t n) {
    static constexpr char d[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { out += d[p[i] >> 4]; out += d[p[i] & 15]; }
    return out;
}
inline std::string Hex(const std::vector<uint8_t>& v) { return Hex(v.data(), v.size()); }
inline std::string Sha256Hex(const uint8_t* p, size_t n) {
    uint8_t h[SHA256_DIGEST_LENGTH];
    SHA256(p, n, h);
    return Hex(h, sizeof(h));
}

inline std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    Check(bool(in), "cannot read " + p.string());
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
inline void WriteFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    Check(bool(out), "cannot write " + p.string());
    out << text;
    out.close();
    Check(bool(out), "write failed " + p.string());
}

// A WAL or shared-memory file next to a closed database means data has not
// been checkpointed into the file itself; refuse instead of reading around it.
inline void RequireNoSidecars(const fs::path& root) {
    for (const auto& rel : DatabaseFiles())
        for (const char* suffix : {"-wal", "-shm", "-journal"}) {
            const fs::path side = root / (rel + suffix);
            Check(!fs::exists(side), "unexpected sidecar file " + side.string());
        }
}

// Raw, immutable read of the file as stored: no PRAGMA, no WAL, no write.
class RawDb {
public:
    explicit RawDb(const fs::path& p) {
        Check(fs::is_regular_file(p), "missing database " + p.string());
        const std::string uri = "file:" + p.string() + "?immutable=1";
        if (sqlite3_open_v2(uri.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
            sqlite3_close(db_);
            Fail("cannot open " + p.string());
        }
    }
    ~RawDb() { sqlite3_close(db_); }
    RawDb(const RawDb&) = delete;
    RawDb& operator=(const RawDb&) = delete;
    sqlite3* get() const { return db_; }
private:
    sqlite3* db_ = nullptr;
};

inline std::string Escape(const unsigned char* text) {
    if (!text) return "NULL";
    std::string out;
    for (const char* c = reinterpret_cast<const char*>(text); *c; ++c) {
        if (*c == '\n') out += "\\n";
        else if (*c == '\t') out += "\\t";
        else if (*c == '\\') out += "\\\\";
        else out += *c;
    }
    return out;
}

// schema.txt: for each DB, SELECT type,name,tbl_name,sql FROM sqlite_master
// ORDER BY 1,2 (triggers included), one tab-separated row per line.
inline std::string SchemaDump(const fs::path& root) {
    RequireNoSidecars(root);
    std::string out;
    for (const auto& rel : DatabaseFiles()) {
        RawDb db(root / rel);
        out += "## " + rel + "\n";
        sqlite3_stmt* q = nullptr;
        Check(sqlite3_prepare_v2(db.get(), "SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY 1,2", -1, &q,
                                 nullptr) == SQLITE_OK, "schema query failed for " + rel);
        int rc;
        while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
            for (int c = 0; c < 4; ++c) {
                if (c) out += '\t';
                out += Escape(sqlite3_column_text(q, c));
            }
            out += '\n';
        }
        sqlite3_finalize(q);
        Check(rc == SQLITE_DONE, "schema read failed for " + rel);
    }
    return out;
}

// Every entry (files and directories) under the data directory, relative,
// sorted. Compared before vs after an open: a relocated store, a copy- or
// migrate-into-a-new-file open, or a leftover sidecar shows up here.
inline std::string TreeDump(const fs::path& root) {
    std::vector<std::string> entries;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        const auto rel = fs::relative(e.path(), root).generic_string();
        entries.push_back((e.is_directory() ? "d " : "f ") + rel);
    }
    std::sort(entries.begin(), entries.end());
    std::string out;
    for (const auto& e : entries) out += e + "\n";
    return out;
}

// Columns an open legitimately rewrites, excluded (by name) from the row
// comparison. Determined from evidence: the generator's fixed-point pass
// (no relocation) and the checker's relocated open (see fixtures README).
struct ExcludedColumn { const char* db; const char* table; const char* column; };
inline const std::vector<ExcludedColumn>& ExcludedColumns() {
    static const std::vector<ExcludedColumn> cols{
        // WalletManager::open(): updateLastOpened(name), unix seconds.
        {"wallet_registry.db", "wallets", "last_opened"},
        // WalletManager::open(): a relocated datadir repairs the stored
        // absolute/relative wallet path to <datadir>/wallets/wallet_<name>.db.
        {"wallet_registry.db", "wallets", "path"},
    };
    return cols;
}
inline bool Excluded(const std::string& db, const std::string& table, const std::string& column) {
    for (const auto& c : ExcludedColumns())
        if (db == c.db && table == c.table && column == c.column) return true;
    return false;
}

inline std::vector<std::string> Strings(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* q = nullptr;
    Check(sqlite3_prepare_v2(db, sql.c_str(), -1, &q, nullptr) == SQLITE_OK, "query failed: " + sql);
    std::vector<std::string> out;
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) out.push_back(Escape(sqlite3_column_text(q, 0)));
    sqlite3_finalize(q);
    Check(rc == SQLITE_DONE, "query read failed: " + sql);
    return out;
}
inline std::string Quote(const std::string& id) {
    std::string out = "\"";
    for (char c : id) { if (c == '"') out += '"'; out += c; }
    return out + "\"";
}

// Every persisted row of every table (sqlite_master type='table', including
// sqlite_sequence), raw immutable read, typed values (blobs as hex), ordered
// by all selected columns. Excluded columns are omitted and named in the
// table header line so the exclusion is visible in every diff.
inline std::string RowDump(const fs::path& root) {
    RequireNoSidecars(root);
    std::string out;
    for (const auto& rel : DatabaseFiles()) {
        RawDb db(root / rel);
        out += "## " + rel + "\n";
        for (const auto& table : Strings(db.get(), "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")) {
            std::vector<std::string> cols, skipped;
            for (const auto& c : Strings(db.get(), "SELECT name FROM pragma_table_info(" +
                                                       std::string("'") + table + "') ORDER BY cid"))
                (Excluded(rel, table, c) ? skipped : cols).push_back(c);
            out += "### " + table + " (" + std::to_string(cols.size()) + " columns";
            for (const auto& s : skipped) out += "; excluded " + s;
            out += ")\n";
            if (cols.empty()) continue;
            std::string list, order;
            for (size_t i = 0; i < cols.size(); ++i) {
                list += (i ? "," : "") + Quote(cols[i]);
                order += (i ? "," : "") + std::to_string(i + 1);
            }
            sqlite3_stmt* q = nullptr;
            const std::string sql = "SELECT " + list + " FROM " + Quote(table) + " ORDER BY " + order;
            Check(sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &q, nullptr) == SQLITE_OK, "row query failed: " + sql);
            int rc;
            while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
                for (int c = 0; c < int(cols.size()); ++c) {
                    if (c) out += '\t';
                    switch (sqlite3_column_type(q, c)) {
                    case SQLITE_NULL: out += "NULL"; break;
                    case SQLITE_INTEGER: out += "i:" + std::to_string(sqlite3_column_int64(q, c)); break;
                    case SQLITE_FLOAT: {
                        char buf[40];
                        std::snprintf(buf, sizeof(buf), "r:%.17g", sqlite3_column_double(q, c));
                        out += buf;
                        break;
                    }
                    case SQLITE_TEXT: out += "t:" + Escape(sqlite3_column_text(q, c)); break;
                    default: {
                        const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(q, c));
                        out += "x:" + Hex(p, size_t(sqlite3_column_bytes(q, c)));
                    }
                    }
                }
                out += '\n';
            }
            sqlite3_finalize(q);
            Check(rc == SQLITE_DONE, "row read failed: " + sql);
        }
    }
    return out;
}

// Persisted database-header fields per file: bytes 18-19 (file format
// write/read version: 1 legacy, 2 WAL -- the persisted journal mode), 20-23
// (reserved space, payload fractions) and 44-67 (schema format, default cache
// size, auto-vacuum root, text encoding, user_version, incremental vacuum,
// application_id). Change counters (24-27, 92-99) and the schema cookie
// (40-43) legitimately move on any write and are not compared.
inline std::string HeaderDump(const fs::path& root) {
    RequireNoSidecars(root);
    std::string out;
    for (const auto& rel : DatabaseFiles()) {
        const auto bytes = ReadFile(root / rel);
        Check(bytes.size() >= 100 && bytes.compare(0, 16, std::string("SQLite format 3\0", 16)) == 0,
              "not a SQLite database: " + rel);
        const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
        out += rel + "\t18-19=" + Hex(p + 18, 2) + "\t20-23=" + Hex(p + 20, 4) + "\t44-67=" + Hex(p + 44, 24) + "\n";
    }
    return out;
}

inline json PragmaValue(sqlite3* db, const std::string& pragma) {
    sqlite3_stmt* q = nullptr;
    Check(sqlite3_prepare_v2(db, ("PRAGMA " + pragma).c_str(), -1, &q, nullptr) == SQLITE_OK,
          "PRAGMA " + pragma + " failed");
    json v;
    if (sqlite3_step(q) == SQLITE_ROW) {
        if (sqlite3_column_type(q, 0) == SQLITE_INTEGER) v = sqlite3_column_int64(q, 0);
        else v = Escape(sqlite3_column_text(q, 0));
    }
    sqlite3_finalize(q);
    return v;
}

// Per DB: file-level PRAGMAs, plus the connection PRAGMAs that the current
// build's open_sqlite() applies when it opens that file.
inline json PragmaDump(const fs::path& root) {
    json all = json::object();
    for (const auto& rel : DatabaseFiles()) {
        auto opened = open_sqlite((root / rel).string());
        if (opened.rc != SQLITE_OK) {
            if (opened.db) sqlite3_close(opened.db);
            Fail("open_sqlite failed for " + rel + ": " + opened.errmsg);
        }
        json p = json::object();
        for (const char* name : {"user_version", "application_id", "page_size", "auto_vacuum", "encoding",
                                 "journal_mode", "synchronous", "locking_mode", "foreign_keys", "temp_store",
                                 "wal_autocheckpoint", "busy_timeout", "cache_size", "mmap_size"})
            p[name] = PragmaValue(opened.db, name);
        Check(sqlite3_close(opened.db) == SQLITE_OK, "close failed for " + rel);
        all[rel] = p;
    }
    return all;
}

#ifdef SEP_FIXTURE_ORCHARD
// Synthetic regtest Orchard domain (regtest itself has no Orchard
// activation). Fixed so the encrypted snapshot AAD is reproducible.
inline constexpr uint32_t kOrchardBranch = 0x53455046;  // "SEPF"
inline constexpr uint32_t kOrchardActivation = 1000;
inline constexpr uint32_t kDeliveryHeight = kTipHeight + 1;
inline uint256 Filled(uint8_t b) { uint256 h; std::fill(h.begin(), h.end(), b); return h; }
inline dinero::orchard::SigningDomain OrchardDomain() {
    dinero::orchard::SigningDomain d;
    d.network_code = 2;
    uint256 genesis;
    Check(uint256::FromHex(dinero::Params().genesis_hash, genesis) && !genesis.IsNull(), "regtest genesis");
    std::copy(genesis.begin(), genesis.end(), d.genesis_wire.begin());
    d.branch_id = kOrchardBranch;
    return d;
}
inline dinero::orchard::Hash WalletIdBytes(const std::string& identity) {
    // "DNWI01:" + 64 lowercase hex characters.
    Check(identity.size() == 7 + 64 && identity.rfind("DNWI01:", 0) == 0, "delivery identity format");
    dinero::orchard::Hash id{};
    for (size_t i = 0; i < 32; ++i) id[i] = uint8_t(std::stoul(identity.substr(7 + 2 * i, 2), nullptr, 16));
    return id;
}
inline json Progress(const std::optional<dinero::RuntimeIndexProgress>& p) {
    if (!p) return nullptr;
    return json{{"cursor_sequence", p->cursor.sequence}, {"cursor_digest", p->cursor.digest.GetHex()},
                {"origin_hash", p->origin_hash.GetHex()}, {"origin_height", p->origin_height},
                {"tip_hash", p->tip_hash.GetHex()}, {"tip_height", p->tip_height}};
}
#endif

// Opens the data directory read-write the way the daemon does (WalletManager
// on the datadir, open(name), UTXOIndex on <blockchain>/utxo, address load)
// and derives every compared field. All handles are closed before return.
inline json Derive(const fs::path& root, const std::string& schema_path) {
    json out;
    out["format"] = 1;
    out["config"] = kConfig;
    {
        dinero::WalletManager wm(root, nullptr, schema_path);
        json reg;
        reg["wallets"] = wm.listWallets();
        reg["exists"] = wm.exists(kWalletName);
        out["registry"] = reg;

        wm.open(kWalletName);
        dinero::UTXOIndex index(UtxoDb(root).string());
        Check(index.Initialize(), "UTXOIndex::Initialize failed");
        wm.setUTXOIndex(&index);
        wm.LoadAddressesIntoUTXOIndex();

        json w;
        w["name"] = wm.current();
        w["blockchain_height"] = wm.getBlockchainHeight();
        w["primary_address"] = wm.getPrimaryAddress();
        json addrs = json::array();
        auto rows = wm.listAddresses(true);
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return std::tie(a.account, a.change, a.index, a.address) < std::tie(b.account, b.change, b.index, b.address);
        });
        for (const auto& r : rows) {
            const auto path = wm.getDerivationPath(r.script_pubkey);
            addrs.push_back({{"address", r.address}, {"account", r.account}, {"change", r.change},
                             {"index", r.index}, {"external", r.external}, {"type", r.type},
                             {"script_pubkey", r.script_pubkey}, {"label", r.label ? json(*r.label) : json(nullptr)},
                             {"derivation_path", path ? json(*path) : json(nullptr)},
                             {"is_mine", wm.isAddressMine(r.address)},
                             {"index_owner", index.IsOurScript([&] {
                                  std::vector<uint8_t> s;
                                  for (size_t i = 0; i + 1 < r.script_pubkey.size(); i += 2)
                                      s.push_back(uint8_t(std::stoul(r.script_pubkey.substr(i, 2), nullptr, 16)));
                                  return s;
                              }()).value_or("")}});
        }
        w["addresses"] = addrs;
        const auto watch = WatchScript();
        const auto watch_path = wm.getWatchScriptPath(watch);
        w["watch_scripts"] = json::array({{{"script_pubkey", Hex(watch)},
                                           {"path", watch_path ? json(*watch_path) : json(nullptr)},
                                           {"is_mine", wm.isScriptMine(Hex(watch))},
                                           {"index_owner", index.IsOurScript(watch).value_or("")}}});
        const auto bal = wm.getBalance();
        w["balance"] = {{"confirmed", bal.confirmed}, {"unconfirmed", bal.unconfirmed}, {"immature", bal.immature},
                        {"total", bal.total}, {"spendable", bal.spendable}, {"utxo_count", bal.utxo_count},
                        {"immature_utxo_count", bal.immature_utxo_count}};
        json utxos = json::array();
        auto unspent = wm.listUnspentUTXOs(0);
        std::sort(unspent.begin(), unspent.end(),
                  [](const auto& a, const auto& b) { return std::tie(a.txid, a.vout) < std::tie(b.txid, b.vout); });
        for (const auto& u : unspent)
            utxos.push_back({{"txid", u.txid}, {"vout", u.vout}, {"amount_una", u.amount_una},
                             {"address", u.address}, {"height", u.height}, {"confirmations", u.confirmations},
                             {"spendable", u.spendable}, {"is_coinbase", u.is_coinbase},
                             {"script_pubkey", u.script_pubkey}, {"derivation_path", u.derivation_path}});
        w["utxos"] = utxos;
        out["wallet"] = w;

        json ix;
        json iu = json::array();
        auto rows_ix = index.GetUnspentUTXOs();
        std::sort(rows_ix.begin(), rows_ix.end(), [](const auto& a, const auto& b) {
            return std::make_pair(a.txid.AsUint256().GetHex(), a.vout) < std::make_pair(b.txid.AsUint256().GetHex(), b.vout);
        });
        for (const auto& u : rows_ix)
            iu.push_back({{"txid", u.txid.AsUint256().GetHex()}, {"vout", u.vout}, {"value_una", u.value.GetUna()},
                          {"spk", Hex(u.spk)}, {"path", u.path}, {"height", u.height},
                          {"spend_height", u.spend_height ? json(*u.spend_height) : json(nullptr)},
                          {"is_coinbase", u.is_coinbase}, {"owner_kind", int(u.owner_kind)}});
        ix["utxos"] = iu;
        ix["balance_una"] = index.GetBalance().GetUna();
        const auto count = index.GetUTXOCount();
        Check(count.isOk(), "UTXOIndex::GetUTXOCount failed");
        ix["utxo_count"] = count.value();
        out["utxo_index"] = ix;

#ifdef SEP_FIXTURE_ORCHARD
        json orch;
        const auto domain = OrchardDomain();
        {
            auto lease = wm.AcquireDatabaseLease();
            const auto identity = lease->EnsureDeliveryIdentity();  // existing; sets synchronous=FULL
            const auto wid = WalletIdBytes(identity);
            Check(lease->ReadDeliveryIdentity() == wid, "delivery identity mismatch");
            auto seed = lease->CopyRecoverySeed(lease->Session());
            dinero::orchard::WalletSnapshotStore store(
                lease->Database(), {dinero::orchard::WalletNetwork::Regtest, domain.genesis_wire, wid, 0},
                seed->Bytes());
            const auto saved = store.Read();
            Check(saved.has_value(), "Orchard snapshot missing");
            orch["snapshot_revision"] = saved->revision;
            orch["snapshot_sha256"] = Sha256Hex(saved->state.Bytes().data(), saved->state.Bytes().size());
            json retained = json::array();
            for (uint64_t r = 1; r < saved->revision; ++r) {
                const auto old = store.ReadRetained(r);
                retained.push_back({{"revision", old.revision},
                                    {"sha256", Sha256Hex(old.state.Bytes().data(), old.state.Bytes().size())}});
            }
            orch["retained"] = retained;
            const auto keys = dinero::orchard::WalletKeys::FromSeed(seed->Bytes(), 0);
            const auto account = dinero::wallet::OrchardAccountState::RestoreForRescan(
                saved->state, domain, keys.ExportFullViewingKey(), kOrchardActivation, Filled(0x93));
            const auto receiver = account.IssueReceiver(dinero::orchard::WalletScope::External).second;
            orch["next_external_receiver"] = receiver.EncodeAddress(dinero::orchard::WalletNetwork::Regtest);
        }
        const auto session = wm.AcquireDatabaseLease()->Session();
        orch["ordinary_delivery"] = Progress(dinero::RuntimeOrdinaryDelivery::ReadForWallet(wm, session));
        orch["index_delivery"] = Progress(dinero::RuntimeIndexDelivery::ReadForWallet(wm, index, session));
        out["orchard"] = orch;
#endif
        wm.setUTXOIndex(nullptr);
    }  // UTXOIndex, then WalletManager, close here: WAL checkpointed and removed.
    {
        din::DescriptorStore store(WalletDb(root).string());
        Check(store.initialize(), "DescriptorStore::initialize failed");
        json d = json::array();
        for (const auto& r : store.listDescriptors(false))
            d.push_back({{"id", r.id}, {"descriptor", r.descriptor}, {"checksum", r.checksum}, {"policy", r.policy},
                         {"account", r.account}, {"is_change", r.is_change}, {"is_active", r.is_active},
                         {"created_at", r.created_at}, {"deprecated_at", r.deprecated_at},
                         {"metadata", r.metadata}, {"label", r.label}});
        out["descriptors"] = d;
        store.shutdown();
    }
    out["sqlite"] = PragmaDump(root);
    RequireNoSidecars(root);
    return out;
}

inline std::string Canonical(const json& j) { return j.dump(2) + "\n"; }

}  // namespace sepfx
