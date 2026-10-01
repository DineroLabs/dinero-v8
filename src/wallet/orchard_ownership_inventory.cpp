#include "wallet/orchard_ownership_inventory.h"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <stdexcept>
#include <sqlite3.h>
#include <openssl/crypto.h>
namespace dinero::wallet {
namespace {
using namespace orchard;
void Check(bool v) { if(!v) throw std::runtime_error("Orchard ownership inventory incomplete or invalid"); }
struct Statement {
  sqlite3_stmt* p=nullptr;
  Statement(sqlite3* db,const char* text) {
    if(sqlite3_prepare_v2(db,text,-1,&p,nullptr)!=SQLITE_OK) {
      sqlite3_finalize(p);p=nullptr;Check(false);
    }
  }
  ~Statement(){sqlite3_finalize(p);}
  Statement(const Statement&)=delete;
};
struct ViewingKey {
  FullViewingKeyBytes bytes;
  explicit ViewingKey(const WalletKeys& k):bytes(k.ExportFullViewingKey()){}
  ~ViewingKey(){OPENSSL_cleanse(bytes.data(),bytes.size());}
};
using Locator=std::pair<Hash,uint32_t>;
using RetainedLocator=std::tuple<Hash,uint32_t,uint64_t>;
Hash BlobIdentity(sqlite3_stmt* q,int column) {
  Check(sqlite3_column_type(q,column)==SQLITE_BLOB&&sqlite3_column_bytes(q,column)==32);
  const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q,column));Check(bytes);
  Hash id{};std::copy_n(bytes,32,id.begin());Check(id!=Hash{});return id;
}
bool Exists(sqlite3* db,const char* name) {
  Statement q(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
  Check(sqlite3_bind_text(q.p,1,name,-1,SQLITE_STATIC)==SQLITE_OK);
  const int rc=sqlite3_step(q.p);if(rc==SQLITE_DONE)return false;
  Check(rc==SQLITE_ROW&&sqlite3_step(q.p)==SQLITE_DONE);return true;
}
std::pair<uint32_t,uint64_t> Numbers(sqlite3_stmt* q) {
  Check(sqlite3_column_type(q,1)==SQLITE_INTEGER&&sqlite3_column_type(q,2)==SQLITE_INTEGER);
  const auto account=sqlite3_column_int64(q,1),revision=sqlite3_column_int64(q,2);
  Check(account>=0&&account<0x80000000LL&&revision>0);
  return {static_cast<uint32_t>(account),static_cast<uint64_t>(revision)};
}
} // namespace
OrchardOwnershipInventory::Snapshot OrchardOwnershipInventory::Read(
    sqlite3* db,std::span<const uint8_t> seed) {
  Check(db&&!sqlite3_get_autocommit(db)&&seed.size()==64);
  auto catalog=OrchardAccountCatalog::Read(db,seed);Check(catalog&&catalog->generated);
  Statement identity_row(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
  Check(sqlite3_step(identity_row.p)==SQLITE_ROW);
  const auto wallet_id=BlobIdentity(identity_row.p,0);Check(sqlite3_step(identity_row.p)==SQLITE_DONE);
  Snapshot result{wallet_id,std::move(*catalog),{},{},0,0};
  const bool schema=Exists(db,"orchard_wallet_schema"),current=Exists(db,"orchard_wallet_snapshots"),
             retained=Exists(db,"orchard_wallet_retained");
  Check((!schema&&!current&&!retained)||(schema&&current&&retained));
  if(!schema){Check(result.catalog.accounts.empty());return result;}
  Statement version(db,"SELECT version FROM orchard_wallet_schema WHERE id=1");
  Check(sqlite3_step(version.p)==SQLITE_ROW&&sqlite3_column_type(version.p,0)==SQLITE_INTEGER&&
        sqlite3_column_int64(version.p,0)==1&&sqlite3_step(version.p)==SQLITE_DONE);
  std::map<Locator,uint64_t> rows;
  {
    Statement q(db,"SELECT wallet_id,account,revision FROM orchard_wallet_snapshots ORDER BY wallet_id,account");
    int rc;
    while((rc=sqlite3_step(q.p))==SQLITE_ROW) {
      Check(rows.size()<kMaxRows);const auto id=BlobIdentity(q.p,0);const auto [a,r]=Numbers(q.p);
      Check(rows.emplace(Locator{id,a},r).second);
    }
    Check(rc==SQLITE_DONE);
  }
  std::map<Locator,std::vector<uint64_t>> retained_rows;
  std::set<RetainedLocator> retained_unique;
  {
    Statement q(db,"SELECT wallet_id,account,revision FROM orchard_wallet_retained ORDER BY wallet_id,account,revision");
    int rc;
    while((rc=sqlite3_step(q.p))==SQLITE_ROW) {
      Check(retained_unique.size()<kMaxRows);const auto id=BlobIdentity(q.p,0);const auto [a,r]=Numbers(q.p);
      Check(retained_unique.emplace(id,a,r).second);
      const auto found=rows.find({id,a});Check(found!=rows.end()&&r<found->second);
      retained_rows[{id,a}].push_back(r);
    }
    Check(rc==SQLITE_DONE);
  }
  result.current_rows=rows.size();result.retained_rows=retained_unique.size();
  std::set<Locator> claimed;
  std::set<std::pair<Hash,uint32_t>> current_inputs;
  const auto claim=[&](const WalletStorageIdentity& id,uint64_t revision) {
    const Locator key{id.wallet_id,id.account};const auto found=rows.find(key);
    Check(found!=rows.end()&&found->second==revision&&claimed.insert(key).second);
  };
  for(const auto& entry:result.catalog.accounts) {
    const SigningDomain domain{entry.network,entry.genesis,entry.branch};
    const WalletStorageIdentity identity{static_cast<WalletNetwork>(entry.network),entry.genesis,wallet_id,entry.account};
    const auto keys=WalletKeys::FromSeed(seed,entry.account);ViewingKey fvk(keys);
    OrchardArchiveReader archive(db,identity,domain,seed);
    auto captured=archive.CaptureCurrent(fvk.bytes,entry.activation);
    claim(identity,captured.revision);
    Account account{entry,identity,std::move(captured),{}, {}};
    // Authenticate every retained account payload, including rows outside the
    // immediate undo chain. All declared parent links must be present, older
    // and terminate through this finite strictly decreasing revision graph.
    WalletSnapshotStore store(db,identity,seed);
    std::map<uint64_t,uint64_t> parents;
    parents.emplace(account.current.revision,account.current.metadata.ParentSnapshotRevision());
    const auto normal_history=retained_rows.find({wallet_id,entry.account});
    if(normal_history!=retained_rows.end())for(const auto revision:normal_history->second) {
      auto saved=store.ReadRetained(revision);
      auto metadata=OrchardAccountMetadata::Read(saved.state,domain,fvk.bytes,entry.activation);
      Check(metadata.ParentSnapshotRevision()<revision);
      Check(parents.emplace(revision,metadata.ParentSnapshotRevision()).second);
      account.retained_accounts.push_back({revision,std::move(metadata)});
    }
    for(const auto& [revision,parent]:parents)Check(!parent||(parent<revision&&parents.contains(parent)));
    for(const auto& record:account.current.archive) {
      claim(record.identity,record.record.revision);
      const auto history=retained_rows.find({record.identity.wallet_id,record.identity.account});
      if(history!=retained_rows.end())for(const auto revision:history->second)
        account.retained_archives.push_back({record.id,archive.ReadRetained(record.id,revision)});
    }
    for(const auto& [operation,pending]:account.current.metadata.Operations().Entries())
      for(const auto& input:pending.inputs) {
        Check(current_inputs.emplace(input.txid_wire,input.output_index).second);
        result.current_inputs.push_back({entry.account,operation,input});
      }
    result.accounts.push_back(std::move(account));
  }
  // This also refuses present/retained rows when the generated catalog is empty.
  Check(claimed.size()==rows.size());
  return result;
}
} // namespace dinero::wallet
