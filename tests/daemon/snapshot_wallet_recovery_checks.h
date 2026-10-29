#pragma once
#include "wallet/wallet_worker.h"
#include "wallet/p2mr_address.h"
din::Json rpc_context_wallet_recoversnapshot(const ExecutionContext&,const din::Json&);
namespace dinero {
namespace {
struct SnapshotWalletRecoveryFixture:WalletIndexOwnerFixture {
    SnapshotWalletSourceFixture base{105};
    std::vector<Block> chain;
    assumeutxo::AssumeUtxoReplayEngine replay;
    WalletManager wallet{root/"owned-wallet"};
    CBlockIndex tip;
    std::filesystem::path snapshot=root/"recovery.dat";
    explicit SnapshotWalletRecoveryFixture(uint32_t target=130,uint8_t witness_version=0):chain(BuildDeterministicChain(target,witness_version)),tip(chain.back().header,target){
        std::string error;if(!replay.SeedGenesis(SelectedGenesis(),error))throw std::runtime_error(error);
        for(uint32_t i=0;i<chain.size();++i)if(!replay.ConnectAndAdvance(chain[i],i+1,chain[i].GetHash(),error))throw std::runtime_error(error);
        const auto token=ChainWriteToken::CreateForTesting();
        const auto store=[&](const Block& block,uint32_t height){
            if(db.putHeader(token,block.GetHash(),block.header,height,arith_uint256(height+1))!=Status::Ok||
               db.putBlock(token,block.GetHash(),block)!=Status::Ok||db.putHeightIndex(token,height,block.GetHash())!=Status::Ok)
                throw std::runtime_error("Recovery fixture source storage");
        };
        store(SelectedGenesis(),0);for(uint32_t i=0;i<chain.size();++i)store(chain[i],i+1);
        tip.chainwork=ChainworkToHex(arith_uint256(target+1));
        if(db.setTip(token,tip.hash,target,arith_uint256(target+1))!=Status::Ok||db.setValidatedTip(token,tip.hash,target)!=Status::Ok)
            throw std::runtime_error("Recovery fixture selected storage");
        ShieldedStateStartupTestAccess::BoundaryState(*source,tip,replay);
        if(!source->GetAssumeUtxoLifecycle()->OnSnapshotLoaded(base.blocks.back().GetHash(),105))throw std::runtime_error("Recovery fixture lifecycle");
        {auto index=ChainstateService::AcquireWalletIndexUse(source);index->Index().ApplyAtomically([&]{
            if(!index->Index().SetMetadata("wallet_snapshot_recovery_base_height","105"))throw std::runtime_error("Recovery fixture base");});
         wallet.setUTXOIndex(&index->Index());}
        const auto bytes=base.Bytes(base.coins);{std::ofstream output(snapshot,std::ios::binary);output.write(bytes.data(),bytes.size());}
        std::dynamic_pointer_cast<ConfigService>(context.config)->Set("assumeutxo_snapshot",snapshot.string());
        wallet.create("same-wallet");wallet.open("same-wallet");
        wallet.addWatchScript(chain[24].vtx[0].vout[0].scriptPubKey,"m/84'/1'/0'/0/0",false);
        if(target>=110)wallet.addWatchScript(chain[109].vtx[0].vout[0].scriptPubKey,"m/84'/1'/0'/0/1",false);
    }
    int64_t Count(const char* sql){auto lease=wallet.AcquireDatabaseLease();sqlite3_stmt* q=nullptr;
        if(sqlite3_prepare_v2(lease->Database(),sql,-1,&q,nullptr)!=SQLITE_OK)throw std::runtime_error("Recovery fixture read");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> row(q,sqlite3_finalize);
        if(sqlite3_step(q)!=SQLITE_ROW)throw std::runtime_error("Recovery fixture row");return sqlite3_column_int64(q,0);}
};
}
TEST(SnapshotWalletRecovery, RestoresMatureBaseCoinAndLaterMiningWithoutReplacingWallet){
    SnapshotWalletRecoveryFixture f;std::string error;
    ASSERT_EQ(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error),std::optional<uint32_t>(130))<<error;
    EXPECT_EQ(f.wallet.getCurrentWalletName(),"same-wallet");EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos WHERE is_spent=0"),2);
    EXPECT_GT(f.wallet.getBalance().confirmed,0);EXPECT_GT(f.wallet.getBalance().immature,0);
    const auto history=f.wallet.getTransactionHistory();ASSERT_EQ(history.size(),1u);EXPECT_TRUE(history.front().is_coinbase);
    EXPECT_TRUE(f.wallet.snapshotRecoveryAlreadyApplied(110,f.chain[109].GetHash().GetHex()));
    EXPECT_THROW(f.wallet.snapshotRecoveryAlreadyApplied(110,std::string(64,'f')),std::runtime_error);
    f.wallet.open("same-wallet");
    ASSERT_EQ(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error),std::optional<uint32_t>(130))<<error;
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos WHERE is_spent=0"),2);EXPECT_EQ(f.wallet.getTransactionHistory().size(),1u);
}
TEST(SnapshotWalletRecovery, TaprootReceiptKeepsCanonicalAddressAndDoesNotDuplicateWorkerHistory){
    SnapshotWalletRecoveryFixture f(130,1);
    const auto& tx=f.chain[109].vtx[0];const auto id=tx.GetTxid().AsUint256().GetHex();
    const auto& script=tx.vout[0].scriptPubKey;const std::vector<uint8_t> program(script.begin()+2,script.end());
    const auto address=bech32::Encode(Params().hrp,1,program,bech32::Encoding::BECH32M);
    {auto index=ChainstateService::AcquireWalletIndexUse(f.source);WalletWorker worker(&index->Index(),&f.wallet);
     WalletWorkerTestAccess::Connect(worker,f.wallet,110,f.chain[109]);}
    const auto live=f.wallet.getTransactionHistory();ASSERT_EQ(live.size(),1u);EXPECT_EQ(live.front().address,address);
    ASSERT_TRUE(f.wallet.addTransaction(id,address,1.0,"generate",true,"worker label",1,110));
    std::string error;ASSERT_TRUE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error))<<error;
    auto history=f.wallet.getTransactionHistory();ASSERT_EQ(history.size(),1u);
    EXPECT_EQ(history.front().address,address);EXPECT_EQ(history.front().label,"worker label");
    EXPECT_TRUE(history.front().is_coinbase);EXPECT_GT(f.wallet.getBalance().immature,0);
    ASSERT_TRUE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error))<<error;
    EXPECT_EQ(f.wallet.getTransactionHistory().size(),1u);
}
TEST(SnapshotWalletRecovery, RepairsOnlyVerifiedTaprootChecksumAliasAndPreservesLabel){
    SnapshotWalletRecoveryFixture f(130,1);
    const auto& tx=f.chain[109].vtx[0];const auto id=tx.GetTxid().AsUint256().GetHex();
    const auto& script=tx.vout[0].scriptPubKey;const std::vector<uint8_t> program(script.begin()+2,script.end());
    const auto address=bech32::Encode(Params().hrp,1,program,bech32::Encoding::BECH32M);
    const auto obsolete=bech32::Encode("din",1,program,bech32::Encoding::BECH32);
    ASSERT_TRUE(f.wallet.addTransaction(id,address,1.0,"generate",true,"",1,110));
    ASSERT_TRUE(f.wallet.addTransaction(id,obsolete,1.0,"generate",true,"retained label",1,110));
    {auto lease=f.wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(lease->Database(),
        "CREATE TRIGGER refuse_receipt BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT,'receipt failure'); END",nullptr,nullptr,nullptr),SQLITE_OK);}
    std::string refused;EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&refused));
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM transactions"),2); // Alias cleanup rolls back too.
    {auto lease=f.wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(lease->Database(),"DROP TRIGGER refuse_receipt",nullptr,nullptr,nullptr),SQLITE_OK);}
    ASSERT_TRUE(f.wallet.addTransaction(std::string(64,'e'),obsolete,2.0,"receive",false,"unrelated",1,1));
    std::string error;ASSERT_TRUE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error))<<error;
    auto history=f.wallet.getTransactionHistory();ASSERT_EQ(history.size(),2u);
    const auto found=std::find_if(history.begin(),history.end(),[&](const auto& row){return row.txid==id;});
    ASSERT_NE(found,history.end());EXPECT_EQ(found->address,address);EXPECT_EQ(found->label,"retained label");
    ASSERT_TRUE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error))<<error;
    EXPECT_EQ(f.wallet.getTransactionHistory().size(),2u);
}
TEST(SnapshotWalletRecovery, P2mrReceiptUsesCanonicalAddressAndKeepsWorkerHistory){
    SnapshotWalletRecoveryFixture f(130,3);
    const auto& tx=f.chain[109].vtx[0];const auto id=tx.GetTxid().AsUint256().GetHex();
    const auto& script=tx.vout[0].scriptPubKey;
    const auto address=wallet::EncodeP2MRAddress(Params().hrp,std::vector<uint8_t>(script.begin()+2,script.end()));
    ASSERT_FALSE(address.empty());
    ASSERT_TRUE(f.wallet.addTransaction(id,address,1.0,"generate",true,"pq receipt",1,110));
    ASSERT_TRUE(f.wallet.addTransaction(id,"script:5320252525252525",1.0,"generate",true,"obsolete fallback",1,110));
    std::string error;ASSERT_TRUE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error))<<error;
    const auto history=f.wallet.getTransactionHistory();ASSERT_EQ(history.size(),1u);
    EXPECT_EQ(history.front().address,address);EXPECT_EQ(history.front().label,"pq receipt");
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos WHERE address LIKE 'script:%'"),0);
}
TEST(SnapshotWalletRecovery, CorruptBodyOrRequiredWalletWriteFailureDoesNotPublishPartialRecovery){
    SnapshotWalletRecoveryFixture f;const auto token=ChainWriteToken::CreateForTesting();
    auto corrupt=f.chain[109];corrupt.vtx[0].vout[0].value=AmountUna::Una(1);
    ASSERT_EQ(f.db.putBlock(token,corrupt.GetHash(),corrupt),Status::Ok);std::string error;
    EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error));EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos"),0);
    ASSERT_EQ(f.db.putBlock(token,f.chain[109].GetHash(),f.chain[109]),Status::Ok);
    {auto lease=f.wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(lease->Database(),
        "CREATE TRIGGER refuse_recovery BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT,'fixture history failure'); END",nullptr,nullptr,nullptr),SQLITE_OK);}
    EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error));EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos"),0);
    EXPECT_EQ(f.wallet.getCurrentBlockchainHeight(),0u);EXPECT_EQ(f.wallet.getCurrentWalletName(),"same-wallet");
}

TEST(SnapshotWalletRecovery, QueuedCoveredConnectCannotLowerRecoveredHeightOrMaturity){
    SnapshotWalletRecoveryFixture f;std::string error;
    ASSERT_EQ(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error),std::optional<uint32_t>(130))<<error;
    const auto before=f.wallet.getBalance();ASSERT_GT(before.confirmed,0);
    auto index=ChainstateService::AcquireWalletIndexUse(f.source);
    WalletWorker worker(&index->Index(),&f.wallet);
    WalletWorkerTestAccess::Connect(worker,f.wallet,110,f.chain[109]);
    EXPECT_EQ(f.wallet.getCurrentBlockchainHeight(),130u);
    EXPECT_DOUBLE_EQ(f.wallet.getBalance().confirmed,before.confirmed);
    EXPECT_EQ(f.wallet.getTransactionHistory().size(),1u);
    f.wallet.onBlockDisconnected(f.chain.back(),130);
    EXPECT_FALSE(f.wallet.snapshotRecoveryAlreadyApplied(130,f.chain.back().GetHash().GetHex()));
}
TEST(SnapshotWalletRecovery, FailedCommitKeepsOldWalletAndRetryRepairsBothStores){
    SnapshotWalletRecoveryFixture f;
    {auto lease=f.wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(lease->Database(),
        "PRAGMA foreign_keys=ON;CREATE TABLE recovery_parent(id INTEGER PRIMARY KEY);"
        "CREATE TABLE recovery_child(id INTEGER REFERENCES recovery_parent(id) DEFERRABLE INITIALLY DEFERRED);"
        "CREATE TRIGGER break_commit AFTER INSERT ON utxos BEGIN INSERT INTO recovery_child VALUES(1); END",nullptr,nullptr,nullptr),SQLITE_OK);}
    std::string error;EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error));
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos"),0);EXPECT_EQ(f.wallet.getCurrentBlockchainHeight(),0u);
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM sqlite_schema WHERE name='wallet_snapshot_recovered_blocks'"),0);
    {auto lease=f.wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(lease->Database(),"DROP TRIGGER break_commit",nullptr,nullptr,nullptr),SQLITE_OK);}
    ASSERT_EQ(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error),std::optional<uint32_t>(130))<<error;
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos WHERE is_spent=0"),2);
    auto index=ChainstateService::AcquireWalletIndexUse(f.source);
    EXPECT_EQ(index->Index().GetUnspentUTXOs().size(),2u);
}
TEST(SnapshotWalletRecovery, StaleSessionAndPendingOutgoingRefuseWithoutReplacingFunds){
    SnapshotWalletRecoveryFixture f;const auto old=f.wallet.AcquireDatabaseLease()->Session();f.wallet.open("same-wallet");
    std::string error;EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error,old));
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos"),0);
    ASSERT_TRUE(f.wallet.addTransaction(std::string(64,'b'),"recipient",-0.1,"send",false,"pending",1,0));
    EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error));
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM transactions"),1);EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos"),0);
}

TEST(SnapshotWalletRecovery, UnavailableOrWrongSnapshotLeavesExistingCoinsUntouched){
    SnapshotWalletRecoveryFixture f;
    ASSERT_TRUE(f.wallet.addUTXO(std::string(64,'a'),0,12345,"sentinel","5120",1,false));
    auto bytes=f.base.Bytes(f.base.coins);bytes[8]^=1;
    {std::ofstream out(f.snapshot,std::ios::binary);out.write(bytes.data(),bytes.size());}
    std::string error;EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error));
    EXPECT_EQ(f.Count("SELECT amount FROM utxos"),12345);
    std::filesystem::remove(f.snapshot);
    EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error));
    EXPECT_EQ(f.Count("SELECT amount FROM utxos"),12345);
}
TEST(SnapshotWalletRecovery, OwnedLegacyMetadataRequiresActualCreationTransaction){
    SnapshotWalletRecoveryFixture f;
    f.wallet.addWatchScript(f.chain[0].vtx[0].vout[0].scriptPubKey,"m/84'/1'/0'/0/2",false);
    auto rows=f.base.coins;
    auto coin=std::find_if(rows.begin(),rows.end(),[](const auto& c){return c.second.height==1;});ASSERT_NE(coin,rows.end());
    coin->second.isCoinbase=false; // Does not change the old v1 leaf hash.
    auto bytes=f.base.Bytes(rows);
    {std::ofstream out(f.snapshot,std::ios::binary);out.write(bytes.data(),bytes.size());}
    std::string error;EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error));
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos"),0);
    bytes=f.base.Bytes(f.base.coins);
    {std::ofstream out(f.snapshot,std::ios::binary);out.write(bytes.data(),bytes.size());}
    ASSERT_TRUE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error))<<error;
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos WHERE is_spent=0"),3);
}
TEST(SnapshotWalletRecovery, RpcRejectsWrongWalletIdentityBeforeRecoveryEffects){
    SnapshotWalletRecoveryFixture f;
    auto service=std::make_shared<WalletService>();ASSERT_TRUE(service->Init(f.context));f.context.wallet=service;
    {auto use=WalletService::AcquireWalletUse(service);use->Wallet().create("original");
     ASSERT_FALSE(use->Wallet().getNewAddress().empty());
     ASSERT_TRUE(use->Wallet().addUTXO(std::string(64,'a'),0,12345,"sentinel","5120",1,false));}
    ExecutionContext request;request.daemon=&f.context;
    auto params=din::obj();params["walletname"]="different";params["expected_first_address"]="different";
    const auto reply=rpc_context_wallet_recoversnapshot(request,params);
    EXPECT_TRUE(reply.isMember("error"));
    {auto use=WalletService::AcquireWalletUse(service);EXPECT_EQ(use->Wallet().getCurrentWalletName(),"original");
     auto lease=use->Wallet().AcquireDatabaseLease();sqlite3_stmt* raw=nullptr;
     ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),"SELECT amount FROM utxos",-1,&raw,nullptr),SQLITE_OK);
     std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> row(raw,sqlite3_finalize);
     ASSERT_EQ(sqlite3_step(raw),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int64(raw,0),12345);}
    service->Stop();f.context.wallet.reset();
}

TEST(SnapshotWalletRecovery, MissingSelectedBodyCannotPublishPartialInventory){
    SnapshotWalletRecoveryFixture f;
    ASSERT_EQ(f.db.deleteBlock(ChainWriteToken::CreateForTesting(),f.chain[109].GetHash()),Status::Ok);
    std::string error;EXPECT_FALSE(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error));
    EXPECT_NE(error.find("waiting for selected"),std::string::npos);
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos"),0);EXPECT_EQ(f.wallet.getCurrentBlockchainHeight(),0u);
}
TEST(SnapshotWalletRecovery, RpcRecoversOnlyExplicitExistingIdentityAndReportsNarrowScope){
    SnapshotWalletRecoveryFixture f;
    auto service=std::make_shared<WalletService>();ASSERT_TRUE(service->Init(f.context));f.context.wallet=service;
    std::string first;
    {auto use=WalletService::AcquireWalletUse(service);auto index=ChainstateService::AcquireWalletIndexUse(f.source);
     auto& wallet=use->Wallet();wallet.setUTXOIndex(&index->Index());wallet.create("existing");
     first=wallet.getNewAddress();ASSERT_FALSE(first.empty());
     wallet.addWatchScript(f.chain[24].vtx[0].vout[0].scriptPubKey,"m/84'/1'/0'/0/0",false);
     wallet.addWatchScript(f.chain[109].vtx[0].vout[0].scriptPubKey,"m/84'/1'/0'/0/1",false);}
    ExecutionContext request;request.daemon=&f.context;
    auto params=din::obj();params["walletname"]="existing";params["expected_first_address"]=first;
    const auto reply=rpc_context_wallet_recoversnapshot(request,params);
    ASSERT_FALSE(reply.isMember("error"))<<reply.toStyledString();
    EXPECT_TRUE(reply["success"].asBool());EXPECT_EQ(reply["recovered_height"].asUInt64(),130u);
    EXPECT_FALSE(reply["whole_wallet_certified"].asBool());
    EXPECT_EQ(reply["scope"].asString(),"transparent-current-coins-and-post-snapshot-history");
    {auto use=WalletService::AcquireWalletUse(service);EXPECT_EQ(use->Wallet().getCurrentWalletName(),"existing");
     EXPECT_GT(use->Wallet().getBalance().confirmed,0);EXPECT_EQ(use->Wallet().getTransactionHistory().size(),1u);}
    service->Stop();f.context.wallet.reset();
}

TEST(SnapshotWalletRecovery, BaseOnlyRecoveryCreatesProgressWithoutInventingHistory){
    SnapshotWalletRecoveryFixture f(105);std::string error;
    ASSERT_EQ(f.source->RecoverTransparentSnapshotWallet(f.wallet,&error),std::optional<uint32_t>(105))<<error;
    EXPECT_EQ(f.Count("SELECT COUNT(*) FROM utxos WHERE is_spent=0"),1);
    EXPECT_EQ(f.Count("SELECT last_scanned_height FROM sync_meta WHERE id=1"),105);
    EXPECT_EQ(f.Count("SELECT birth_height FROM sync_meta WHERE id=1"),106);
    EXPECT_TRUE(f.wallet.getTransactionHistory().empty());
    EXPECT_TRUE(f.wallet.snapshotRecoveryAlreadyApplied(105,f.chain.back().GetHash().GetHex()));
}

TEST(SnapshotWalletRecovery, PhoneStartupDoesNotRewindRecoveredWalletToBootstrapChainHeight){
    SnapshotWalletRecoveryFixture f;
    auto service=std::make_shared<WalletService>();ASSERT_TRUE(service->Init(f.context));f.context.wallet=service;
    {auto use=WalletService::AcquireWalletUse(service);auto index=ChainstateService::AcquireWalletIndexUse(f.source);
     auto& wallet=use->Wallet();wallet.setUTXOIndex(&index->Index());wallet.create("preserved");
     wallet.addWatchScript(f.chain[24].vtx[0].vout[0].scriptPubKey,"m/84'/1'/0'/0/0",false);
     std::string error;ASSERT_TRUE(f.source->RecoverTransparentSnapshotWallet(wallet,&error))<<error;
     ASSERT_GT(wallet.getBalance().confirmed,0);}
    // During restart, the chain owner can still be at bootstrap genesis before
    // it has restored the retained selected snapshot. This is not a reorg proof.
    CBlockIndex bootstrap(SelectedGenesis().header,0);
    ShieldedStateStartupTestAccess::Select(*f.source,bootstrap);
    struct Profile {bool old=GetConfig().utreexo_stateless;~Profile(){GetConfig().utreexo_stateless=old;}} profile;
    GetConfig().utreexo_stateless=true;
    EXPECT_TRUE(service->Start());
    {auto use=WalletService::AcquireWalletUse(service);
     EXPECT_EQ(use->Wallet().getCurrentBlockchainHeight(),130u);
     EXPECT_GT(use->Wallet().getBalance().confirmed,0);}
    service->Stop();f.context.wallet.reset();
}

}
