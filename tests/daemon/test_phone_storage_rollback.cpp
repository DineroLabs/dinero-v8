#include "daemon/orchard_storage_upgrade.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;
using dinero::daemon::RetirePhoneStorageRollback;
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void CreateStore(const fs::path& path, int families) {
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::DB* raw = nullptr;
    Check(rocksdb::DB::Open(options, path.string(), &raw).ok(), "open fixture");
    std::unique_ptr<rocksdb::DB> db(raw);
    for (int i=1; i<families; ++i) {
        rocksdb::ColumnFamilyHandle* handle = nullptr;
        Check(db->CreateColumnFamily({}, "cf"+std::to_string(i), &handle).ok(), "create family");
        Check(db->DestroyColumnFamilyHandle(handle).ok(), "destroy handle");
    }
}
int main() try {
    char temp[] = "/tmp/dinero-phone-rollback-XXXXXX";
    Check(mkdtemp(temp), "mkdtemp");
    const fs::path root(temp), db=root/"blockchain"/"chaindb";
    const fs::path legacy=db.string()+".legacy-v1", retiring=db.string()+".legacy-v1.retiring";
    fs::create_directories(db.parent_path());
    CreateStore(db, 10);
    CreateStore(legacy, 9);
    const auto preserved = legacy/"CURRENT";
    { std::ofstream out(db/"shared.sst"); out << "immutable retained bytes"; }
    fs::create_hard_link(db/"shared.sst",legacy/"shared.sst");
    RetirePhoneStorageRollback(root, db, "mac_fullblock");
    Check(fs::exists(preserved), "desktop lost rollback");
    RetirePhoneStorageRollback(root, db, "");
    Check(fs::exists(preserved), "unnamed profile lost rollback");
    const auto journal=fs::path(db.string()+".upgrade-state");
    { std::ofstream out(journal); out << "swapping\n"; }
    bool refused=false;
    try { RetirePhoneStorageRollback(root, db, "ios_utreexo"); } catch (...) { refused=true; }
    Check(refused && fs::exists(preserved), "incomplete swap lost rollback");
    fs::remove(journal);
    for (const auto* control : {dinero::storage::OrchardHandoffSelected,
                                dinero::storage::OrchardHandoffCompleted}) {
        const auto marker=db.parent_path()/control;
        { std::ofstream out(marker); out << "retained source identity"; }
        refused=false;
        try { RetirePhoneStorageRollback(root,db,"ios_utreexo"); } catch (...) {refused=true;}
        Check(refused && fs::exists(preserved), "activation handoff lost retained pair");
        fs::remove(marker);
    }
    fs::create_symlink(root/"absent",journal);
    refused=false;
    try { RetirePhoneStorageRollback(root,db,"ios_utreexo"); } catch (...) {refused=true;}
    Check(refused && fs::exists(preserved), "ambiguous upgrade marker lost rollback");
    fs::remove(journal);
    bool interrupted=false;
    try {
        RetirePhoneStorageRollback(root, db, "ios_utreexo", [](const char*) {throw std::runtime_error("crash");});
    } catch (...) { interrupted=true; }
    Check(interrupted && !fs::exists(legacy) && fs::exists(retiring), "missing retirement boundary");
    const auto saved = fs::path(db.string()+".saved");
    fs::rename(db,saved);
    Check(!dinero::daemon::IsBrandNewStore(db), "retirement mistaken for a fresh install");
    refused=false;
    try { RetirePhoneStorageRollback(root,db,"ios_utreexo"); } catch (...) {refused=true;}
    Check(refused && fs::exists(retiring), "missing live store lost retirement evidence");
    fs::rename(saved,db);
    // Simulate death in the middle of remove_all, then run the same cleanup.
    fs::remove(retiring/"CURRENT");
    RetirePhoneStorageRollback(root, db, "ios_utreexo");
    Check(!fs::exists(legacy) && !fs::exists(retiring), "partial retirement not retried");
    RetirePhoneStorageRollback(root, db, "ios_utreexo");
    Check(dinero::daemon::storage_upgrade_detail::FamilyCount(db)==10, "live DB changed");
    std::ifstream bytes(db/"shared.sst"); std::string retained;
    std::getline(bytes,retained);
    Check(retained=="immutable retained bytes" && fs::hard_link_count(db/"shared.sst")==1,
          "retiring paired SST damaged live bytes");
    fs::remove_all(db);
    CreateStore(db,9); CreateStore(legacy,9);
    refused=false;
    try { RetirePhoneStorageRollback(root,db,"ios_utreexo"); } catch (...) {refused=true;}
    Check(refused && fs::exists(legacy/"CURRENT"), "unmigrated store lost rollback");
    fs::remove_all(root);
    std::cout << "PASS phone rollback retirement, desktop retention, refusal and interrupted retry\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
