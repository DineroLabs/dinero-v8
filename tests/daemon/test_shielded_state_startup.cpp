// Exercise the actual production loader, including its SQLite reconciliation.
// Synthetic READY fixtures are not receipts for the future migration tool.
#include "../storage/shielded_store_fixture.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/chainparams.h"
#include "consensus/shielded/shielded_root.h"
#include <fstream>
#include <cstring>
#include <sqlite3.h>
#include "daemon/services/config_service.h"
#include "consensus/utxo_snapshot.h"
#include "wallet/wallet_manager.h"
#include "crypto/sha256.h"
#include <iomanip>
#include "daemon/services/logger_service.h"

namespace dinero {
struct ShieldedStateStartupTestAccess {
    static void SnapshotPolicy(ChainstateService& service) {
        service.logger_=std::make_shared<LoggerService>("");
        service.consensus_utxo_set_=std::make_unique<consensus::ConsensusUTXOSet>();
    }
    static void SnapshotPath(ChainstateService& service, const std::filesystem::path& path) {
        service.config_ = std::make_shared<ConfigService>();
        service.config_->Set("assumeutxo_snapshot", path.string());
    }
    static bool Persist(ChainstateService& s) { return s.PersistShieldedState(); }
    static bool Marker(ChainstateService& s,const uint256& h,uint32_t n) { return s.PersistShieldedTipMarker(h,n); }
    static bool Import(ChainstateService& s,const uint256& h,uint32_t n) { return s.PersistImportedShieldedState(h,n); }
    static void Append(ChainstateService& s) { consensus::shielded::Hash h{};h[0]=77;s.shielded_tree_.Append(h); }
    static bool Load(ChainstateService& service, const std::filesystem::path& root) {
        service.datadir_ = root.string();
        service.shielded_frontier_path_ = root / "blockchain/shielded_frontier.bin";
        return service.LoadShieldedState();
    }
};
} // namespace dinero
using namespace shielded_store_fixture;
using namespace dinero;
namespace sh = dinero::consensus::shielded;

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Setup(out.good(), "write legacy file");
}

void Startup(const std::string& fault, bool separated = true) {
    TempDir temp;
    const auto dir = temp.path / "chaindb";
    std::filesystem::create_directory(dir);
    Seed(dir, separated, separated ? std::optional<std::string>(ready) : std::nullopt);
    std::filesystem::create_directory(temp.path / "blockchain");
    sh::CommitmentTree tree;
    sh::Hash note{}; note[31] = 7; tree.Append(note);
    sh::AnchorHistory anchors; anchors.RecordRoot(3, tree.Root());
    const auto frontier = tree.SerializeFrontier();
    const auto anchor_bytes = anchors.SerializePersistenceBytes();
    WriteFile(temp.path / "blockchain/shielded_frontier.bin", frontier);
    WriteFile(temp.path / "blockchain/shielded_anchor_history.bin", anchor_bytes);
    const auto cache_path = temp.path / "blockchain/shielded_nullifiers.db";
    sh::Hash cached{}; cached.fill(99);
    {
        sh::NullifierSet cache; Setup(cache.Open(cache_path.string()) == sh::NullifierSet::OpenResult::Ok, "open cache");
        Setup(cache.Insert(cached, 9), "seed cache");
    }
    if (fault == "unstamped_cache") {
        sqlite3* sql = nullptr;
        Setup(sqlite3_open(cache_path.c_str(), &sql) == SQLITE_OK, "open cache for legacy provenance fixture");
        const auto result = sqlite3_exec(sql, "PRAGMA user_version=0", nullptr, nullptr, nullptr);
        sqlite3_close(sql); Setup(result == SQLITE_OK, "unset cache provenance");
    }
    ChainDB db; Setup(db.init(dir) == Status::Ok, "open fixture");
    const auto token = ChainWriteToken::CreateForTesting();
    ChainDB::ShieldedTipMarker marker;
    marker.height = 3; marker.block_hash.data[0] = 3; marker.tree_size = tree.Size();
    std::vector<sh::NullifierEntry> entries;
    if (fault == "zero_nullifiers") {
        Setup(db.deleteAllShieldedNullifiers(token).ok(), "seed valid zero-nullifier state");
    } else {
        for (uint32_t h : {1, 3}) { sh::NullifierEntry entry; entry.height = h; entry.nullifier.fill(h); entries.push_back(entry); }
    }
    const auto tree_root = tree.Root();
    // Match CurrentShieldedStateSnapshot(), the production marker writer:
    // this field holds the commitment-tree root, not the composite DNRS root.
    std::memcpy(marker.shielded_root.data, tree_root.data(), tree_root.size());
    if (fault == "composite_root") {
        const auto composite = sh::ComputeShieldedRootFromParts(
            std::vector<uint8_t>(tree_root.begin(), tree_root.end()), tree.Size(),
            sh::ComputeNullifierAccumulator(entries), anchors.SerializeBytes());
        Setup(composite.has_value() && *composite != marker.shielded_root,
              "composite root must differ from persisted tree root");
        marker.shielded_root = *composite;
    }
    marker.nullifier_count = entries.size();
    Setup(db.setTip(token, marker.block_hash, marker.height, arith_uint256(1)) == Status::Ok, "seed chain tip");
    if (fault == "count") marker.nullifier_count = 8;
    if (fault == "root") marker.shielded_root.data[0] ^= 1;
    if (fault == "size") marker.tree_size += 1;
    if (fault == "height") marker.height += 1;
    if (fault == "hash") marker.block_hash.data[0] ^= 1;
    Setup(db.putShieldedTipMarker(token, marker) == Status::Ok, "seed shielded marker");
    Setup(db.putShieldedState(token, ChainDB::ShieldedStateRecord::Frontier,
        fault == "frontier" ? "invalid-frontier" : std::string(frontier.begin(), frontier.end())) == Status::Ok, "seed frontier");
    Setup(db.putShieldedState(token, ChainDB::ShieldedStateRecord::AnchorHistory,
        fault == "anchors" ? "invalid-anchors" : std::string(anchor_bytes.begin(), anchor_bytes.end())) == Status::Ok, "seed anchors");
    db.close();
    const auto before = Inspect(dir);
    Setup(db.init(dir) == Status::Ok, "reopen fixture");
    const bool accept = fault.empty() || fault == "zero_nullifiers" || !separated;
    {
        ChainstateService service; service.setChainDB(&db);
        CHECK(ShieldedStateStartupTestAccess::Load(service, temp.path) == accept);
        if (accept) {
            CHECK(service.GetShieldedCommitmentTree()->Root() == tree.Root());
            CHECK(service.GetShieldedNullifierSet()->Size() == entries.size());
            sh::Hash expected{}; expected.fill(3);
            CHECK(service.GetShieldedNullifierSet()->Contains(expected) == !entries.empty());
            CHECK(!service.GetShieldedNullifierSet()->Contains(cached));
        }
    }
    db.close(); CHECK(Inspect(dir) == before);
    if (!accept) {
        sh::NullifierSet cache; CHECK(cache.Open(cache_path.string()) == sh::NullifierSet::OpenResult::Ok);
        CHECK(cache.Size() == 1 && cache.Contains(cached));
        if (fault == "unstamped_cache") CHECK(cache.GetProvenance() == sh::NullifierSet::Provenance::LegacyCandidate);
    }
}

// This is the pre-rehydrate startup boundary, not a complete v5 binding test.
// The payload is a manifest-bound header fixture; LoadSnapshot must still
// authenticate the complete container before it may publish imported coins.
void SnapshotMarkerAboveTip() {
    for (const auto& fault : {"valid", "missing_manifest", "header_height", "late_nullifier"}) {
        TempDir temp;const auto dir=temp.path/"chaindb";
        std::filesystem::create_directory(dir);Seed(dir);
        std::filesystem::create_directory(temp.path/"blockchain");
        ChainDB db;CHECK(db.init(dir)==Status::Ok);const auto token=ChainWriteToken::CreateForTesting();
        CHECK(db.deleteAllShieldedNullifiers(token).ok());
        BlockHeader base{};base.prev_block_hash=uint256::FromHexUnsafe(Params().genesis_hash);base.timestamp=123;
        const auto hash=base.GetHash();
        CHECK(db.setTip(token,base.prev_block_hash,0,arith_uint256(1))==Status::Ok);
        CHECK(db.putHeader(token,hash,base,std::string(fault)=="header_height"?2:3,arith_uint256(2))==Status::Ok);
        sh::CommitmentTree tree;sh::Hash note{};note[0]=9;tree.Append(note);
        sh::AnchorHistory anchors;anchors.RecordRoot(3,tree.Root());
        const auto frontier=tree.SerializeFrontier(),history=anchors.SerializePersistenceBytes();
        CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,
            std::string(frontier.begin(),frontier.end()))==Status::Ok);
        CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,
            std::string(history.begin(),history.end()))==Status::Ok);
        sh::Hash nullifier{};nullifier[0]=11;
        CHECK(db.putShieldedNullifier(token,std::string(fault)=="late_nullifier"?4:3,nullifier.data())==Status::Ok);
        ChainDB::ShieldedTipMarker marker;marker.height=3;marker.block_hash=hash;marker.tree_size=tree.Size();marker.nullifier_count=1;
        const auto tree_root=tree.Root();std::copy(tree_root.begin(),tree_root.end(),marker.shielded_root.begin());
        CHECK(db.putShieldedTipMarker(token,marker)==Status::Ok);
        const auto payload=temp.path/"snapshot.dat";
        {
            std::ofstream out(payload,std::ios::binary);
            const auto put=[&](const auto& value){out.write(reinterpret_cast<const char*>(&value),sizeof(value));};
            put(consensus::SNAPSHOT_MAGIC);put(uint32_t(5));out.write(reinterpret_cast<const char*>(hash.data),32);
            put(uint32_t(3));put(uint64_t(0));put(uint64_t(0));put(uint64_t(0));
            const std::array<char,32> tail{};out.write(tail.data(),tail.size());CHECK(out.good());
        }
        if(std::string(fault)!="missing_manifest") {
            std::ifstream in(payload,std::ios::binary);const std::string bytes(std::istreambuf_iterator<char>(in),{});
            uint8_t digest[32];crypto::CSHA256().Write(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()).Finalize(digest);
            std::ostringstream hex;hex<<std::hex<<std::setfill('0');for(auto b:digest)hex<<std::setw(2)<<unsigned(b);
            std::ofstream out(payload.string()+".manifest.json");
            out<<"{\"height\":3,\"block_hash\":\""<<hash.GetHex()<<"\",\"sha256\":\""<<hex.str()<<"\"}";CHECK(out.good());
        }
        db.close();const auto before=Inspect(dir);
        for(unsigned reopen=0;reopen<2;++reopen) {
            CHECK(db.init(dir)==Status::Ok);
            {
                ChainstateService service;service.setChainDB(&db);
                ShieldedStateStartupTestAccess::SnapshotPath(service,payload);
                CHECK(ShieldedStateStartupTestAccess::Load(service,temp.path)==(std::string(fault)=="valid"));
                if(std::string(fault)=="valid") {
                    CHECK(service.GetShieldedCommitmentTree()->Root()==tree.Root());
                    CHECK(service.GetShieldedNullifierSet()->Contains(nullifier));
                    CHECK(db.getTip()->height==0); // Startup loading must not promote history.
                }
            }
            db.close();CHECK(Inspect(dir)==before);
        }
    }
    std::cout<<"PASS snapshot_marker_above_tip\n";
}

void RetiredPersistence(bool separated=true) {
    TempDir temp;const auto dir=temp.path/"chaindb";std::filesystem::create_directory(dir);Seed(dir,separated,separated?std::optional<std::string>(ready):std::nullopt);
    std::filesystem::create_directory(temp.path/"blockchain");
    ChainDB db;CHECK(db.init(dir)==Status::Ok);const auto token=ChainWriteToken::CreateForTesting();
    uint256 parent,child,other;parent.data[0]=3;child.data[0]=4;other.data[0]=5;
    sh::CommitmentTree tree;sh::AnchorHistory anchors;anchors.RecordRoot(3,tree.Root());
    const auto frontier=tree.SerializeFrontier(),history=anchors.SerializePersistenceBytes();
    const auto tree_root=tree.Root();uint256 root;std::copy(tree_root.begin(),tree_root.end(),root.begin());
    CHECK(db.deleteAllShieldedNullifiers(token).ok());
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,{frontier.begin(),frontier.end()})==Status::Ok);
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{history.begin(),history.end()})==Status::Ok);
    CHECK(db.putShieldedTipMarker(token,{3,parent,root,0,0})==Status::Ok);
    CHECK(db.setTip(token,parent,3,arith_uint256(3))==Status::Ok);
    CHECK(db.setValidatedTip(token,parent,3)==Status::Ok);
    WriteFile(temp.path/"blockchain/shielded_frontier.bin",frontier);
    WriteFile(temp.path/"blockchain/shielded_anchor_history.bin",history);
    ChainstateService service;service.setChainDB(&db);
    CHECK(ShieldedStateStartupTestAccess::Load(service,temp.path));
    CHECK(ShieldedStateStartupTestAccess::Persist(service));
    CHECK(ShieldedStateStartupTestAccess::Marker(service,parent,3));
    CHECK(ShieldedStateStartupTestAccess::Import(service,parent,3));
    if(!separated)return; // Old-schema writes keep their historical behavior.
    const auto composite=service.ComputeShieldedRoot();CHECK(composite);
    storage::LegacyRetirementState retired{{2,other,1,4,3,parent,*composite,37,root,0,0},4,child,parent};
    rocksdb::WriteBatch batch;
    CHECK(db.stageLegacyRetirementConnect(token,std::nullopt,retired,batch)==Status::Ok);
    CHECK(db.setTip(token,child,4,arith_uint256(4),&batch)==Status::Ok);
    CHECK(db.setValidatedTip(token,child,4,&batch)==Status::Ok);
    CHECK(db.writeBatch(token,std::move(batch),true)==Status::Ok);
    const auto before=Inspect(dir);
    const auto unchanged=[&] {
        CHECK(Inspect(dir)==before);
        for(const auto& entry:std::vector<std::pair<std::string,std::vector<uint8_t>>>{
                {"shielded_frontier.bin",frontier},{"shielded_anchor_history.bin",history}}) {
            std::ifstream in(temp.path/"blockchain"/entry.first,std::ios::binary);
            const std::vector<uint8_t> actual((std::istreambuf_iterator<char>(in)),{});
            CHECK(actual==entry.second);
        }
    };
    CHECK(ShieldedStateStartupTestAccess::Persist(service));unchanged();
    CHECK(ShieldedStateStartupTestAccess::Marker(service,child,4));unchanged();
    CHECK(!ShieldedStateStartupTestAccess::Marker(service,parent,3));unchanged();
    CHECK(!ShieldedStateStartupTestAccess::Marker(service,other,4));unchanged();
    CHECK(!ShieldedStateStartupTestAccess::Import(service,child,4));unchanged();
    ShieldedStateStartupTestAccess::Append(service);
    CHECK(!ShieldedStateStartupTestAccess::Persist(service));unchanged();
    db.close();CHECK(!ShieldedStateStartupTestAccess::Persist(service));
    CHECK(db.init(dir)==Status::Ok);unchanged();
    CHECK(ShieldedStateStartupTestAccess::Load(service,temp.path));
    rocksdb::WriteBatch undo;
    CHECK(db.stageLegacyRetirementDisconnect(token,retired,undo)==Status::Ok);
    CHECK(db.setTip(token,parent,3,arith_uint256(3),&undo)==Status::Ok);
    CHECK(db.setValidatedTip(token,parent,3,&undo)==Status::Ok);
    CHECK(db.writeBatch(token,std::move(undo),true)==Status::Ok);
    CHECK(ShieldedStateStartupTestAccess::Persist(service));
    CHECK(ShieldedStateStartupTestAccess::Marker(service,parent,3));
}

// Entry-delivery fixture only, not a consensus-qualified snapshot container.
// The production source is exercised with actual wallet writes and progress.
void SnapshotWalletImport() {
    TempDir temp;
    WalletManager wallet(temp.path);
    wallet.create("snapshot"); wallet.open("snapshot");
    std::vector<uint8_t> script(34, 0x42); script[0]=0x51; script[1]=0x20;
    wallet.addWatchScript(script, "m/86'/1'/0'/0/0", false);
    auto scalar = [&](const char* sql) {
        sqlite3_stmt* raw = nullptr;
        CHECK(sqlite3_prepare_v2(wallet.getCurrentDatabase(),sql,-1,&raw,nullptr)==SQLITE_OK);
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(raw,sqlite3_finalize);
        CHECK(sqlite3_step(raw)==SQLITE_ROW);
        const auto result=sqlite3_column_int64(raw,0);
        CHECK(sqlite3_step(raw)==SQLITE_DONE); return result;
    };
    const auto height=wallet.getCurrentBlockchainHeight();
    const auto progress=scalar("SELECT last_scanned_height FROM sync_meta WHERE id=1");
    ChainstateService service;
    const auto path=temp.path/"snapshot.dat";
    ShieldedStateStartupTestAccess::SnapshotPath(service,path);
    CHECK(service.RescanWalletFromSnapshotUTXOs(wallet,10)==-1);
    auto write = [&](uint64_t count) {
        std::ofstream out(path,std::ios::binary|std::ios::trunc);
        const auto put=[&](const auto& value) { out.write(reinterpret_cast<const char*>(&value),sizeof(value)); };
        consensus::SnapshotMetadata header;
        header.magic=consensus::SNAPSHOT_MAGIC;header.version=consensus::SNAPSHOT_VERSION_V4;
        header.block_hash.data[0]=10;header.block_height=10;header.utxo_count=count;
        header.timestamp=1;header.reserved=0;
        put(header.magic);put(header.version);out.write(reinterpret_cast<const char*>(header.block_hash.data),32);
        put(header.block_height);put(header.utxo_count);put(header.timestamp);put(header.reserved);
        uint256 txid;txid.data[0]=44;
        out.write(reinterpret_cast<const char*>(txid.data),32);
        put(uint32_t(0));put(uint64_t(5000));put(uint32_t(script.size()));
        out.write(reinterpret_cast<const char*>(script.data()),script.size());put(uint32_t(4));put(uint8_t(0));
        CHECK(out.good());
    };
    // The producer reaches EOF before its advertised entry count. No partial
    // wallet import or scan-progress publication may escape that I/O failure.
    write(2);
    CHECK(service.RescanWalletFromSnapshotUTXOs(wallet,10)==-1);
    CHECK(scalar("SELECT COUNT(*) FROM utxos")==0);
    CHECK(scalar("SELECT last_scanned_height FROM sync_meta WHERE id=1")==progress);
    CHECK(wallet.getCurrentBlockchainHeight()==height);
    write(1);
    CHECK(service.RescanWalletFromSnapshotUTXOs(wallet,11)==-1);
    CHECK(scalar("SELECT COUNT(*) FROM utxos")==0);
    CHECK(service.RescanWalletFromSnapshotUTXOs(wallet,10)==1);
    CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE amount=5000 AND height=4 AND is_spent=0")==1);
    CHECK(scalar("SELECT last_scanned_height FROM sync_meta WHERE id=1")==10);
    wallet.open("snapshot");
    CHECK(wallet.getCurrentBlockchainHeight()==10);
    CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE amount=5000")==1);
    CHECK(service.RescanWalletFromSnapshotUTXOs(wallet,10)==1);
    CHECK(scalar("SELECT COUNT(*) FROM utxos")==1);
    std::cout << "PASS snapshot_wallet_import\n";
}

void OrchardSnapshotPolicyRuntime() {
    struct Restore {ChainParams params;~Restore(){MutableParams()=params;}} restore{Params()};
    MutableParams().orchard_activation_height=150000;MutableParams().orchard_branch_id=0xa1b2c3d4;
    MutableParams().state_commitment_activation_height=UINT32_MAX;
    MutableParams().orchard_catalog_commitment_activation_height=UINT32_MAX;
    TempDir temp;const auto database=temp.path/"chaindb";std::filesystem::create_directory(database);Seed(database);
    ChainDB db;CHECK(db.init(database)==Status::Ok);const auto token=ChainWriteToken::CreateForTesting();
    uint256 hash;hash.data[0]=7;CHECK(db.setTip(token,hash,150000,arith_uint256{1})==Status::Ok);
    db.close();const auto before=Inspect(database);CHECK(db.init(database)==Status::Ok);
    const auto file=temp.path/"snapshot.dat";
    const auto write=[&](uint32_t version,uint32_t height) {
        std::ofstream out(file,std::ios::binary|std::ios::trunc);
        const auto put=[&](const auto& value){out.write(reinterpret_cast<const char*>(&value),sizeof(value));};
        put(consensus::SNAPSHOT_MAGIC);put(version);out.write(reinterpret_cast<const char*>(hash.data),32);
        put(height);put(uint64_t(0));put(uint64_t(0));put(uint64_t(0));
        const std::array<char,32> padding{};out.write(padding.data(),padding.size());CHECK(out.good());
    };
    const auto read=[&] {std::ifstream in(file,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
    for(unsigned reopen=0;reopen<2;++reopen) {
        {
            ChainstateService service;service.setChainDB(&db);ShieldedStateStartupTestAccess::SnapshotPolicy(service);
            for(uint32_t version:{3,4,5})for(uint32_t height:{150000,150001}) {
                write(version,height);const auto payload=read();const auto result=service.LoadSnapshot(file);
                CHECK(!result.success&&result.utxos_imported==0&&
                    result.error_message.find("Legacy snapshot container cannot bootstrap Orchard")!=std::string::npos);
                CHECK(read()==payload);
            }
            write(6,150000);const auto dormant=service.LoadSnapshot(file);
            CHECK(!dormant.success&&dormant.error_message.find("active DNORST02")!=std::string::npos);
            const auto sentinel=read();const auto exported=service.ExportSnapshot(file);
            CHECK(!exported.success&&exported.error_message.find("active DNORST02")!=std::string::npos);
            CHECK(read()==sentinel); // Refusal precedes opening/truncating output.
        }
        db.close();CHECK(Inspect(database)==before);CHECK(db.init(database)==Status::Ok);
    }
    std::cout<<"PASS orchard_snapshot_policy_runtime: pre-body refusal, unchanged storage/reopen, preserved export destination\n";
}

int main(int argc, char** argv) {
    SelectParams(Chain::REGTEST);
    if(argc==2&&std::string(argv[1])=="snapshot_marker_above_tip") {
        try{SnapshotMarkerAboveTip();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    }
    if(argc==2&&std::string(argv[1])=="orchard_snapshot_policy_runtime") {
        try{OrchardSnapshotPolicyRuntime();return 0;}
        catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    }
    if (argc == 2 && std::string(argv[1]) == "snapshot_wallet_import") {
        try { SnapshotWalletImport(); return 0; }
        catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
    const std::vector<std::string> cases{"valid", "zero_nullifiers", "unstamped_cache", "frontier", "anchors", "count", "root", "composite_root", "size", "height", "hash", "legacy_frontier", "legacy_anchors", "retired_persistence", "legacy_persistence"};
    if (argc == 2 && std::string(argv[1]) == "--list") {
        for (const auto& name : cases) std::cout << name << '\n'; return 0;
    }
    unsigned passed = 0, failed = 0;
    for (const auto& name : cases) {
        if (argc == 2 && name != argv[1]) continue;
        try {
            const bool legacy_case = name.rfind("legacy_", 0) == 0;
            if(name=="retired_persistence" || name=="legacy_persistence")RetiredPersistence(name=="retired_persistence");
            else Startup(name == "valid" ? "" : legacy_case ? name.substr(7) : name, !legacy_case);
            ++passed; std::cout << "PASS " << name << '\n';
        } catch (const Failure& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
        catch (const std::exception& e) { std::cerr << "SETUP ERROR " << name << ": " << e.what() << '\n'; return 2; }
    }
    if (!passed && !failed) return 2;
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
