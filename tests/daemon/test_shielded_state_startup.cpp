// Exercise the actual production loader, including its SQLite reconciliation.
// Synthetic READY fixtures are not receipts for the future migration tool.
#include "../storage/shielded_store_fixture.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/chainparams.h"
#include "consensus/shielded/shielded_root.h"
#include <fstream>
#include <cstring>
#include <sqlite3.h>

namespace dinero {
struct ShieldedStateStartupTestAccess {
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

int main(int argc, char** argv) {
    SelectParams(Chain::REGTEST);
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
