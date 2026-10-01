#pragma once
#include "wallet/orchard_archive_reader.h"
static void CheckOrchardArchiveCapture(sqlite3* db,const std::string& path,
    const SigningDomain& domain,const FullViewingKeyBytes& fvk,
    const OrchardAccountState& current,uint64_t revision,
    const Bytes& first_body,const Bytes& second_body) {
  using Reader=dinero::wallet::OrchardArchiveReader;
  Reader reader(db,Identity(domain),domain,seed);
  Fails([&]{(void)reader.CaptureCurrent(fvk,20001);}); // Requires caller snapshot.
  const auto validate=[&](const Reader::Captured& captured) {
    Require(captured.revision==revision&&captured.metadata.Archive()==current.Archive());
    Require(captured.metadata.Delivery()==current.Delivery()&&
            captured.metadata.ParentSnapshotRevision()==current.ParentSnapshotRevision());
    Require(captured.metadata.Operations().Entries().empty());
    Require(captured.archive.size()==2&&captured.archive[0].id==Hash{2}&&captured.archive[1].id==Hash{1});
    for(size_t i=0;i<2;++i) {
      const auto& entry=captured.archive[i];
      Require(entry.record.sequence==2-i&&entry.identity.wallet_id!=Identity(domain).wallet_id&&entry.identity.account==0);
      Require(entry.record.operation.Entries().size()==1&&entry.record.operation.Entries().contains(entry.id));
      const auto& operation=entry.record.operation.Entries().at(entry.id);
      Require(operation.transaction==(i==0?second_body:first_body)&&!operation.inputs.empty());
      Require(entry.record.observation.outcome==OrchardAccountState::OperationOutcome::Confirmed);
    }
    Require(captured.archive[0].record.previous==Hash{1}&&captured.archive[1].record.previous==Hash{});
  };
  Sql(db,"BEGIN IMMEDIATE;");const auto changes=sqlite3_total_changes64(db);
  auto captured=reader.CaptureCurrent(fvk,20001);validate(captured);
  Require(!sqlite3_get_autocommit(db)&&sqlite3_total_changes64(db)==changes);
  Sql(db,"ROLLBACK;");validate(captured); // Owned output survives end of snapshot.
  // Reopen the actual encrypted file read-only; authentication/chain capture
  // must not depend on the writable connection or create schema.
  sqlite3* reopened=nullptr;
  Require(sqlite3_open_v2(path.c_str(),&reopened,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK);
  {
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> close(reopened,sqlite3_close);
    Sql(reopened,"PRAGMA synchronous=FULL;BEGIN;");
    Reader readonly(reopened,Identity(domain),domain,seed);
    validate(readonly.CaptureCurrent(fvk,20001));
    WalletSnapshotStore readonly_store(reopened,Identity(domain),seed);
    Fails([&]{(void)readonly_store.StageReplace(revision,current.Encode());});
    Fails([&]{(void)readonly_store.StageReplaceRetaining(revision,current.Encode());});
    Fails([&]{WalletSnapshotStore::InitializeSchemaUnderTransaction(reopened);});
    Require(!sqlite3_get_autocommit(reopened)&&sqlite3_total_changes64(reopened)==0);
    Sql(reopened,"ROLLBACK;");
  }
  std::cout<<"PASS archive capture current owner and complete predecessor chain\n";

  Sql(db,"BEGIN IMMEDIATE;");
  auto wrong_seed=seed;wrong_seed[0]^=1;
  Reader other_seed(db,Identity(domain),domain,wrong_seed);
  Fails([&]{(void)other_seed.CaptureCurrent(fvk,20001);});
  auto wrong_identity=Identity(domain);wrong_identity.wallet_id[0]^=1;
  Reader other_owner(db,wrong_identity,domain,seed);
  Fails([&]{(void)other_owner.CaptureCurrent(fvk,20001);});
  auto wrong_fvk=fvk;wrong_fvk[0]^=1;
  Fails([&]{(void)reader.CaptureCurrent(wrong_fvk,20001);});
  Fails([&]{(void)reader.CaptureCurrent(fvk,20002);});
  Require(!sqlite3_get_autocommit(db)&&sqlite3_total_changes64(db)==changes);
  {
    struct Reset {sqlite3* db;~Reset(){sqlite3_set_authorizer(db,nullptr,nullptr);}} reset{db};
    Require(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
      return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_snapshots"?SQLITE_DENY:SQLITE_OK;
    },nullptr)==SQLITE_OK);
    Fails([&]{(void)reader.CaptureCurrent(fvk,20001);});
    Require(!sqlite3_get_autocommit(db));
  }
  validate(reader.CaptureCurrent(fvk,20001));
  Require(sqlite3_total_changes64(db)==changes);Sql(db,"ROLLBACK;");
  std::cout<<"PASS archive capture wrong owners and denied reads preserve transaction\n";

  // Use the actual retaining store on a genuine reached record. Existing
  // archive writers may not retain older revisions; this does not invent them.
  Fails([&]{(void)reader.ReadRetained(Hash{1},1);});
  Sql(db,"BEGIN IMMEDIATE;");
  {
    const auto owners=reader.CaptureCurrent(fvk,20001);
    const auto& first=owners.archive[1];
    WalletSnapshotStore record_store(db,first.identity,seed);
    const auto prior=record_store.Read();Require(bool(prior));
    const auto next=record_store.StageReplaceRetaining(prior->revision,prior->state);
    const auto history=reader.ReadRetained(first.id,prior->revision);
    Require(history.revision==prior->revision&&history.sequence==first.record.sequence&&
            history.previous==first.record.previous&&history.observation==first.record.observation);
    Require(history.operation.Entries().at(first.id).transaction==first_body);
    Fails([&]{(void)reader.ReadRetained(first.id,next);});
    Fails([&]{(void)reader.ReadRetained(Hash{99},prior->revision);});
    {
      struct Reset {sqlite3* db;~Reset(){sqlite3_set_authorizer(db,nullptr,nullptr);}} reset{db};
      Require(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;
      },nullptr)==SQLITE_OK);
      Fails([&]{(void)reader.ReadRetained(first.id,prior->revision);});
    }
    Require(reader.ReadRetained(first.id,prior->revision).revision==prior->revision);
    Require(!sqlite3_get_autocommit(db));
  }
  Sql(db,"ROLLBACK;");
  Sql(db,"BEGIN IMMEDIATE;");validate(reader.CaptureCurrent(fvk,20001));Sql(db,"ROLLBACK;");
  std::cout<<"PASS retained archive reader exact record and caller ownership\n";
}
