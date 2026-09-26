#pragma once
#include "wallet/runtime_index_delivery.h"
#include "wallet/utxo_index.h"
#include "wallet/wallet_manager.h"
#include <future>
#include <chrono>
#include <thread>
namespace dinero {
struct RuntimeIndexDeliveryTestAccess {
 static auto Read(dinero::UTXOIndex& i,const std::string& id){return RuntimeIndexDelivery::Read(i,id);}
 static auto Apply(dinero::UTXOIndex& i,const std::string& id,const RuntimeOutboxEvent& e){return RuntimeIndexDelivery::Apply(i,id,e);}
};
}

static void IndexDeliveryChecks(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path) {
    TempDir wallet;const auto file=(wallet.path/"index.sqlite").string();
    dinero::UTXOIndex index(file);CHECK(index.Initialize());
    auto historical=HistoricalDeliveryParent();historical.header=RequiredValue(db.getHeader(c.parent_hash));
    CHECK(historical.GetHash()==c.parent_hash);
    const auto register_scripts=[&](dinero::UTXOIndex& target) {
        target.RegisterAddress(historical.vtx.front().vout.front().scriptPubKey,"m/86'/1448'/0'/0/0");
        for(const auto& tx:block.Transactions()) {
            if(tx.IsOrchard())for(const auto& output:tx.Orchard().Outputs())target.RegisterAddress(output.script_pub_key,"m/86'/1448'/0'/0/0");
            else for(const auto& output:tx.Historical().vout)target.RegisterAddress(output.scriptPubKey,"m/86'/1448'/0'/0/0");
        }
    };register_scripts(index);
    const auto undo=RequiredValue(db.getUndo(c.block_hash));
    size_t baseline=0;
    for(const auto& spent:undo.spent)if(spent.height<c.height) {
        CHECK(index.AddUTXO(WalletUTXO(TxId(spent.prev_txid),spent.prev_vout,AmountUna::Una(spent.value),spent.scriptPubKey,"m/86'/1448'/0'/0/0",spent.height,spent.is_coinbase)));++baseline;
    }
    const auto& old_output=historical.vtx.front().vout.front();
    CHECK(index.AddUTXO(WalletUTXO(historical.vtx.front().GetTxid(),0,old_output.value,old_output.scriptPubKey,"m/86'/1448'/0'/0/0",c.height-1,true)));++baseline;
    const auto failure=[&](const std::function<void()>& f,const char* text) {
        bool refused=false;try{f();}catch(const std::exception& e){refused=std::string(e.what()).find(text)!=std::string::npos;}CHECK(refused);
    };
    AnnotatedRecursiveMutex lock;std::lock_guard<AnnotatedRecursiveMutex> guard(lock);
    const auto event=ReadRuntimeOutboxUnderLock(db,c).events.front();CHECK(event.cursor.sequence==1);
    CHECK(!RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture"));
    // The production entry points derive identity from a real pinned wallet.
    { WalletManager owner(wallet.path/"manager");owner.create("delivery");owner.open("delivery");
      dinero::UTXOIndex bound((wallet.path/"bound.sqlite").string());CHECK(bound.Initialize());
      auto session=owner.AcquireDatabaseLease()->Session();
      CHECK(!RuntimeIndexDelivery::ReadForWallet(owner,bound,session));
      CHECK(RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event).cursor==event.cursor);
      owner.open("delivery");
      failure([&]{(void)RuntimeIndexDelivery::ReadForWallet(owner,bound,session);},"selection changed");
      failure([&]{RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event);},"selection changed");
      session=owner.AcquireDatabaseLease()->Session();
      CHECK(RuntimeIndexDelivery::ReadForWallet(owner,bound,session)->cursor==event.cursor);
      CHECK(RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event).cursor==event.cursor);
      { WalletManager namesake(wallet.path/"namesake");namesake.create("delivery");
        const auto other_session=namesake.AcquireDatabaseLease()->Session();
        failure([&]{(void)RuntimeIndexDelivery::ReadForWallet(namesake,bound,other_session);},"ownership changed");
        failure([&]{RuntimeIndexDelivery::ApplyForWallet(namesake,bound,other_session,event);},"ownership changed"); }
      owner.create("other");owner.open("delivery");session=owner.AcquireDatabaseLease()->Session();
      // Hold the real index owner while the production reader waits for it.
      // Its wallet lease must remain pinned for that entire wait.
      std::future<std::optional<RuntimeIndexProgress>> reader;
      std::future<void> switcher;
      std::promise<void> switch_started, switch_acquired;
      auto started=switch_started.get_future();auto acquired=switch_acquired.get_future();
      bool pinned=false, switch_blocked=false;
      auto* sqlite_mutex=sqlite3_db_mutex(owner.getCurrentDatabase());
      bound.ApplyAtomically([&] {
          reader=std::async(std::launch::async,[&]{return RuntimeIndexDelivery::ReadForWallet(owner,bound,session);});
          const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
          do {
              const int rc=sqlite3_mutex_try(sqlite_mutex);
              if(rc==SQLITE_BUSY){pinned=true;break;}
              CHECK(rc==SQLITE_OK);sqlite3_mutex_leave(sqlite_mutex);
              std::this_thread::yield();
          } while(std::chrono::steady_clock::now()<deadline);
          switcher=std::async(std::launch::async,[&]{
              switch_started.set_value();
              { auto competing_lease=owner.AcquireDatabaseLease();switch_acquired.set_value(); }
              owner.open("other");
          });
          started.wait();
          switch_blocked=acquired.wait_for(std::chrono::milliseconds(100))==std::future_status::timeout;
      });
      CHECK(reader.get()->cursor==event.cursor);switcher.get();
      CHECK(pinned && switch_blocked);
      failure([&]{RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event);},"selection changed");
      session=owner.AcquireDatabaseLease()->Session();
      failure([&]{(void)RuntimeIndexDelivery::ReadForWallet(owner,bound,session);},"ownership changed");
      failure([&]{RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event);},"ownership changed"); }

    sqlite3* raw=nullptr;CHECK(sqlite3_open(file.c_str(),&raw)==SQLITE_OK);
    struct Close {sqlite3* db;~Close(){sqlite3_close(db);}} close{raw};
    const auto sql=[&](const char* text){CHECK(sqlite3_exec(raw,text,nullptr,nullptr,nullptr)==SQLITE_OK);};
    // A receipt write failure must roll back every input/output effect.
    sql("CREATE TRIGGER reject_receipt BEFORE INSERT ON utxo_metadata WHEN NEW.key='runtime_delivery:v1:receipt' BEGIN SELECT RAISE(ABORT,'receipt failure'); END");
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",event);},"Index delivery statement failed");
    CHECK(!RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture") && index.GetUTXOCount().value()==baseline);
    for(const auto& spent:undo.spent)if(spent.height<c.height)CHECK(!index.GetUTXO(TxId(spent.prev_txid),spent.prev_vout)->spend_height);
    CHECK(!index.GetUTXO(TxId(H(250)),UINT32_MAX)); // Finish the reusable SELECT before external schema DDL.
    sql("DROP TRIGGER reject_receipt");
    // Required row errors and deferred COMMIT rejection cannot advance progress.
    sql("CREATE TRIGGER reject_output BEFORE INSERT ON wallet_utxos BEGIN SELECT RAISE(ABORT,'output failure'); END");
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",event);},"Index delivery output failed");
    CHECK(!RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture") && index.GetUTXOCount().value()==baseline);
    sql("DROP TRIGGER reject_output");
    sql("CREATE TABLE delivery_parent(id INTEGER PRIMARY KEY); CREATE TABLE delivery_child(id INTEGER REFERENCES delivery_parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER reject_commit AFTER INSERT ON utxo_metadata WHEN NEW.key='runtime_delivery:v1:receipt' BEGIN INSERT INTO delivery_child VALUES(99); END");
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",event);},"Wallet UTXO write commit failed");
    CHECK(!RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture") && index.GetUTXOCount().value()==baseline);
    for(const auto& spent:undo.spent)if(spent.height<c.height)CHECK(!index.GetUTXO(TxId(spent.prev_txid),spent.prev_vout)->spend_height);
    CHECK(!index.GetUTXO(TxId(H(250)),UINT32_MAX));
    sql("DROP TRIGGER reject_commit");
    CHECK(index.BeginTransaction() && index.SetMetadata("borrowed","pending"));
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",event);},"write ownership unavailable");
    CHECK(index.GetMetadata("borrowed")==std::optional<std::string>("pending"));
    CHECK(index.RollbackTransaction() && !index.GetMetadata("borrowed"));
    const auto first=RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",event);
    CHECK(first.cursor==event.cursor && first.tip_hash==c.block_hash && first.origin_hash==c.parent_hash);
    CHECK(RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",event).cursor==first.cursor);
    const auto count=index.GetUTXOCount().value();CHECK(count>baseline);
    for(const auto& spent:undo.spent)CHECK(index.GetUTXO(TxId(spent.prev_txid),spent.prev_vout)->spend_height==int(c.height));
    failure([&]{(void)RuntimeIndexDeliveryTestAccess::Read(index,"other-wallet");},"ownership changed");
    CHECK(!index.SetMetadata("runtime_delivery:v1:receipt","forged") && !index.DeleteMetadata("runtime_delivery:v1:receipt"));
    CHECK(index.Initialize());register_scripts(index);
    CHECK(RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture")->cursor==first.cursor);
    auto skipped=event;skipped.cursor.sequence+=2;skipped.previous_digest=first.cursor.digest;
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",skipped);},"source discontinuity");
    index.RegisterAddress({0x51,0x51},"m/86'/1448'/0'/0/1");
    failure([&]{(void)RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture");},"ownership changed");
    index.ClearRegisteredAddresses();register_scripts(index);
    // Actual indexed rollback gives the next checked source event.
    BlockStorage files;CHECK(files.init(path)==Status::Ok);
    auto disk_index=DiskIndex(db,block.Header(),c.height);const auto parent=RequiredValue(db.getHeader(c.parent_hash));
    ConsensusUTXOSet live;live.ReplaceForestGuarded(forest);live.SetBestBlock(c.block_hash,c.height);
    CHECK(db.forEachUTXO([&](const uint256& hash,uint32_t n,const Coin& coin){CHECK(live.AddCoin(OutPoint(TxId(hash),n),MemoryCoin(coin)));return true;})==Status::Ok);
    auto rollback=PreparedOrchardChainstateWrite::DisconnectIndexed(lock,db,token,files,disk_index,live,c,block,parent,forest,true);
    rollback->Commit();rollback.reset();
    const auto down=ReadRuntimeOutboxUnderLock(db,c,first.cursor).events.front();
    CHECK(down.direction==RuntimeBlockDirection::Disconnect);
    const auto second=RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",down);
    CHECK(second.cursor==down.cursor && second.tip_hash==c.parent_hash && index.GetUTXOCount().value()==baseline);
    for(const auto& spent:undo.spent)if(spent.height<c.height)CHECK(!index.GetUTXO(TxId(spent.prev_txid),spent.prev_vout)->spend_height);
    // Cross below activation through the existing historical source log. These
    // are real historical body/effect identities, not historical consensus replay.
    auto next_cursor=second.cursor;
    CHECK(!index.GetUTXO(TxId(H(250)),UINT32_MAX));
    for(auto direction:{RuntimeBlockDirection::Disconnect,RuntimeBlockDirection::Connect}) {
        auto prepared=PreparedHistoricalRuntimeOutbox::PrepareUnderLock(db,c,historical,c.height-1,direction);CHECK(prepared);
        rocksdb::WriteBatch canonical;
        Tip(db,direction==RuntimeBlockDirection::Connect?historical.GetHash():historical.header.prev_block_hash,
            direction==RuntimeBlockDirection::Connect?c.height-1:c.height-2,canonical);
        prepared->StageOrTerminateUnderLock(db,canonical);Commit(db,canonical);
        const auto old_event=ReadRuntimeOutboxUnderLock(db,c,next_cursor,1).events.front();CHECK(!old_event.IsOrchardProfile());
        next_cursor=RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",old_event).cursor;
        CHECK(index.GetUTXO(historical.vtx.front().GetTxid(),0).has_value()==(direction==RuntimeBlockDirection::Connect));
        CHECK(index.GetUTXOCount().value()==baseline-(direction==RuntimeBlockDirection::Disconnect?1:0));
    }
    UtreexoForest parent_forest;std::string error;CHECK(storage::RestoreHistoricalForest(db,c.height-1,parent_forest,error)==Status::Ok);
    auto reconnect=PreparedOrchardChainstateWrite::ConnectIndexed(lock,db,token,files,disk_index,live,c,block,parent,parent_forest,{},true,false,FixtureRetirement(c));
    reconnect->Commit();reconnect.reset();
    const auto again=ReadRuntimeOutboxUnderLock(db,c,next_cursor).events.front();
    CHECK(RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",again).cursor==again.cursor && index.GetUTXOCount().value()==count);
    CHECK(index.Initialize());register_scripts(index);
    CHECK(RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture")->cursor==again.cursor);
    // Missing invalidation guards refuse even exact replay rather than being repaired silently.
    std::string trigger;
    { sqlite3_stmt* stmt=nullptr;CHECK(sqlite3_prepare_v2(raw,"SELECT sql FROM sqlite_master WHERE name='runtime_delivery_UPDATE'",-1,&stmt,nullptr)==SQLITE_OK);
      CHECK(sqlite3_step(stmt)==SQLITE_ROW);trigger=reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
      CHECK(sqlite3_step(stmt)==SQLITE_DONE);sqlite3_finalize(stmt); }
    sql("DROP TRIGGER runtime_delivery_UPDATE");
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",again);},"guard unavailable");
    sql(trigger.c_str());
    CHECK(RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture")->cursor==again.cursor);
    // A zero-owned-output index must invalidate its receipt on reset, too.
    { dinero::UTXOIndex empty((wallet.path/"empty.sqlite").string());CHECK(empty.Initialize());
      CHECK(RuntimeIndexDeliveryTestAccess::Apply(empty,"empty-wallet",event).cursor==event.cursor);
      CHECK(empty.GetUTXOCount().value()==0 && empty.ClearAll());
      failure([&]{(void)RuntimeIndexDeliveryTestAccess::Read(empty,"empty-wallet");},"invalidated"); }
    // Existing production writers cannot leave a stale successful receipt.
    const auto& spent=undo.spent.front();CHECK(index.SpendUTXO(TxId(spent.prev_txid),spent.prev_vout,c.height));
    failure([&]{(void)RuntimeIndexDeliveryTestAccess::Read(index,"wallet-fixture");},"invalidated");
    CHECK(index.ClearAll());
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,"wallet-fixture",event);},"invalidated");
}
