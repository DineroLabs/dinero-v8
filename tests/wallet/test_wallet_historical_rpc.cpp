#include "vault/wallet_withdrawal_dispatch.h"
#include "vault/state_snapshot.h"
#include "rpc/wallet_request_dispatch.h"
#include "consensus/chainparams.h"
#include "daemon/services/assumeutxo_replay.h"
#include "crypto/wallet_crypto.h"
#include <openssl/crypto.h>
#include <gtest/gtest.h>
#include "common/test_logger.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/config_service.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/interfaces/tx_ingress.h"
#include <functional>
#include "rpc/rpc_registry.h"
#include "wallet/transaction_builder.h"
#include "wallet/wallet_transaction_signer.h"
#include "consensus/pq/p2mr_consensus.h"
#include "primitives/transaction.h"
#include "wallet/taproot_keys.h"
#include "consensus/script_interpreter.h"
#include "dinero/core/common/AddressCodec.h"
#include "util/hex.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <openssl/sha.h>
#include <sqlite3.h>
#include <filesystem>
#include <unistd.h>
#include <cstring>

namespace dinero {
struct WalletBatchPaymentTestAccess {
    static void InstallIndex(ChainstateService& chain,std::unique_ptr<UTXOIndex> index) {chain.utxo_index_=std::move(index);}
    static bool SelectedHeld(ChainstateService& s) {return s.activation_mutex_.HeldByCurrentThread();}
    static void InstallValidatedParent(ChainstateService& s,CBlockIndex& tip,const assumeutxo::AssumeUtxoReplayEngine& replay) {
        if(!s.utxo_index_ || !s.consensus_utxo_set_ || !s.block_validator_ || s.consensus_utxo_set_->GetSetSize()!=replay.ProvenUtxos().size())
            throw std::runtime_error("actual Init-created canonical pool fixture owners required");
        for(const auto& [point,coin]:replay.ProvenUtxos()) {
            const auto* loaded=s.consensus_utxo_set_->GetCoin(point);
            if(!loaded || loaded->value!=coin.value || loaded->scriptPubKey!=coin.scriptPubKey || loaded->height!=coin.height ||
               loaded->isCoinbase!=coin.isCoinbase || loaded->is_confidential!=coin.is_confidential || loaded->commitment!=coin.commitment)
                throw std::runtime_error("pool source loaded coin differs from completed validation");
        }
        s.consensus_utxo_set_->ReplaceForestGuarded(*replay.Forest());s.consensus_utxo_set_->SetBestBlock(tip.hash,tip.height);s.active_tip_=&tip;
    }
};
}
void registerV7PqWalletMethods();
din::Json rpc_context_wallet_sendmany(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_getbalance(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_getwalletinfo(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_snapshot(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_listunspent(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_listpendingpayments(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_signrawtransaction(const ExecutionContext&,const din::Json&);
namespace {
class HistoricalRpc : public ::testing::Test {
protected:
    dinero::NullLogger logger;DaemonContext daemon;ExecutionContext ctx;
    std::shared_ptr<dinero::WalletService> service;
    std::unique_ptr<dinero::UTXOIndex> index;std::filesystem::path root;
    dinero::CanonicalWalletUTXO old,modern,hd;
    std::string modern_address;
    static void sql(sqlite3* db,const std::string& text) {
        char* e=nullptr;int rc=sqlite3_exec(db,text.c_str(),nullptr,nullptr,&e);
        std::string error=e?e:"";sqlite3_free(e);if(rc!=SQLITE_OK)throw std::runtime_error(error);
    }
    static dinero::CanonicalWalletUTXO coin(const std::vector<uint8_t>& script,uint32_t n) {
        dinero::CanonicalWalletUTXO c;c.txid=dinero::uint256::FromHexUnsafe(std::string(63,'0')+"1");c.vout=n;
        c.value=dinero::AmountUna::Una(100000);c.height=1;c.spk=script;return c;
    }
    void SetUp() override {
        char tmp[]="/tmp/dinero-historical-rpc-XXXXXX";ASSERT_NE(mkdtemp(tmp),nullptr);root=tmp;
        auto config=std::make_shared<dinero::ConfigService>();config->Set("datadir",root.string());daemon.config=config;daemon.logger_interface=&logger;
        service=std::make_shared<dinero::WalletService>();daemon.wallet=service;ASSERT_TRUE(service->Init(daemon));
        index=std::make_unique<dinero::UTXOIndex>((root/"index.db").string());ASSERT_TRUE(index->Initialize());
        auto& w=service->get();w.setUTXOIndex(index.get());w.create("other");w.create("owner");
        std::array<uint8_t,32> secret{},internal{},output{},tweak{};secret.back()=67;int parity=0;
        ASSERT_TRUE(dinero::TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity));
        std::array<uint8_t,33> material{};std::copy(internal.begin(),internal.end(),material.begin());::SHA256(material.data(),material.size(),tweak.data());
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> secp(secp256k1_context_create(SECP256K1_CONTEXT_VERIFY),secp256k1_context_destroy);
        secp256k1_xonly_pubkey pub{},xout{};secp256k1_pubkey point{};
        ASSERT_TRUE(secp256k1_xonly_pubkey_parse(secp.get(),&pub,internal.data()));ASSERT_TRUE(secp256k1_xonly_pubkey_tweak_add(secp.get(),&point,&pub,tweak.data()));
        ASSERT_TRUE(secp256k1_xonly_pubkey_from_pubkey(secp.get(),&xout,nullptr,&point));ASSERT_TRUE(secp256k1_xonly_pubkey_serialize(secp.get(),output.data(),&xout));
        const auto address=AddressCodec::encodeP2TR(Network::MAIN,std::vector<uint8_t>(output.begin(),output.end()));

        std::vector<uint8_t> script{0x51,0x20};script.insert(script.end(),output.begin(),output.end());old=coin(script,0);
        std::vector<uint8_t> key(32);key.back()=91;modern_address=w.importPrivateKey(key,"modern");ASSERT_FALSE(modern_address.empty());
        auto spk=w.getScriptPubKeyForAddress(modern_address);ASSERT_TRUE(spk);ASSERT_TRUE(util::unhex(*spk,script));modern=coin(script,1);auto path=w.getWatchScriptPath(script);ASSERT_TRUE(path);modern.path=*path;
        const auto hd_address=w.getNewAddress();spk=w.getScriptPubKeyForAddress(hd_address);ASSERT_TRUE(spk);ASSERT_TRUE(util::unhex(*spk,script));hd=coin(script,2);path=w.getDerivationPath(*spk);ASSERT_TRUE(path);hd.path=*path;
        sql(w.getCurrentDatabase(),"INSERT INTO imported_keys(address,private_key_enc,label) VALUES('"+address+"','"+util::hex(std::vector<uint8_t>(secret.begin(),secret.end()))+"','preserved import')");
        w.encryptWallet("historical-rpc");w.open("owner");w.unlockWallet("historical-rpc",0);ctx.daemon=&daemon;ctx.walletName="owner";
    }
    void TearDown() override {service->get().setUTXOIndex(nullptr);service->Stop();index.reset();std::filesystem::remove_all(root);}
    static dinero::Transaction transaction(const std::vector<dinero::CanonicalWalletUTXO>& coins) {
        dinero::Transaction tx;tx.version=2;uint64_t amount=0;
        for(const auto& c:coins){dinero::TxInput in;in.prevout=dinero::TxOutPoint(dinero::TxId(c.txid),c.vout);tx.vin.push_back(in);amount+=c.value.GetUna();}
        dinero::TxOutput out;out.value=dinero::AmountUna::Una(amount-1000);out.scriptPubKey=coins[0].spk;tx.vout.push_back(out);return tx;
    }
    static din::Json params(const std::vector<dinero::CanonicalWalletUTXO>& coins) {
        din::Json p(Json::arrayValue);p.append(transaction(coins).SerializeHex(false));din::Json prev(Json::arrayValue);
        for(const auto& c:coins){din::Json row;row["txid"]=c.GetTxIdHex();row["vout"]=c.vout;row["scriptPubKey"]=util::hex(c.spk);row["amount"]=double(c.value.GetUna())/1e8;prev.append(row);}p.append(prev);return p;
    }
    void verify(const dinero::Transaction& tx,const std::vector<dinero::CanonicalWalletUTXO>& coins) {
        ASSERT_EQ(tx.vin.size(),coins.size());
        for(size_t i=0;i<coins.size();++i){
            dinero::consensus::ScriptExecutionContext c(&tx,i,coins[i].value.GetUna(),dinero::consensus::SCRIPT_VERIFY_WITNESS|dinero::consensus::SCRIPT_VERIFY_TAPROOT);
            for(const auto& u:coins){c.all_amounts.push_back(u.value.GetUna());c.all_scriptpubkeys.push_back(u.spk);c.all_confidential_flags.push_back(0);c.all_input_commitments.push_back({});}
            dinero::consensus::ScriptError error;ASSERT_FALSE(tx.vin[i].witness.empty());
            EXPECT_TRUE(dinero::consensus::VerifyScript(dinero::consensus::Script(tx.vin[i].scriptSig),dinero::consensus::Script(coins[i].spk),tx.vin[i].witness,c,error));
            c.all_amounts[i]-=1;c.sighash_cache.clear();EXPECT_FALSE(dinero::consensus::VerifyScript(dinero::consensus::Script(tx.vin[i].scriptSig),dinero::consensus::Script(coins[i].spk),tx.vin[i].witness,c,error));
        }
    }
};
TEST_F(HistoricalRpc, ActualRawHandlerPreservesMixedOrigins) {
    auto& w=service->get();const auto changes=sqlite3_total_changes(w.getCurrentDatabase());const std::vector<dinero::CanonicalWalletUTXO> coins{old,modern,hd};
    auto result=rpc_context_wallet_signrawtransaction(ctx,params(coins));ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();ASSERT_TRUE(result.isMember("hex"));
    dinero::Transaction tx;ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(tx,result["hex"].asString()));verify(tx,coins);
    EXPECT_FALSE(result["complete"].asBool()); // no selected chainstate in this component fixture
    EXPECT_TRUE(old.path.empty());EXPECT_FALSE(w.getDerivationPath(util::hex(old.spk)));EXPECT_FALSE(w.getWatchScriptPath(old.spk));EXPECT_EQ(sqlite3_total_changes(w.getCurrentDatabase()),changes);
    auto again=rpc_context_wallet_signrawtransaction(ctx,params({old}));ASSERT_TRUE(again.isMember("hex"));ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(tx,again["hex"].asString()));verify(tx,{old});
}
struct SigningHook {
    dinero::WalletManager& wallet;bool fired=false,lock_refused=false,selection_refused=false;
    static int trace(unsigned,void* raw,void* stmt,void*) {
        auto& h=*static_cast<SigningHook*>(raw);const char* text=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));
        if(h.fired || !text || !std::strstr(text,"FROM imported_keys"))return 0;h.fired=true;
        try{h.wallet.lockWallet();}catch(const std::logic_error&){h.lock_refused=true;}
        try{h.wallet.open("other");}catch(const std::logic_error&){h.selection_refused=true;}
        return 0;
    }
};
TEST_F(HistoricalRpc, ActualRawSessionAndBorrowedTransactionRefusal) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();SigningHook hook{w};sqlite3_trace_v2(db,SQLITE_TRACE_STMT,SigningHook::trace,&hook);
    auto result=rpc_context_wallet_signrawtransaction(ctx,params({old}));sqlite3_trace_v2(db,0,nullptr,nullptr);
    ASSERT_TRUE(result.isMember("hex"))<<result.toStyledString();EXPECT_TRUE(hook.fired);EXPECT_TRUE(hook.lock_refused);EXPECT_TRUE(hook.selection_refused);
    dinero::Transaction tx;ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(tx,result["hex"].asString()));verify(tx,{old});
    ctx.walletName="other";result=rpc_context_wallet_signrawtransaction(ctx,params({old}));EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("hex"));ctx.walletName="owner";
    sql(db,"BEGIN");result=rpc_context_wallet_signrawtransaction(ctx,params({old}));EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(sqlite3_get_autocommit(db));sql(db,"ROLLBACK");
    w.lockWallet();result=rpc_context_wallet_signrawtransaction(ctx,params({old}));EXPECT_TRUE(result.isMember("error"));w.unlockWallet("historical-rpc",0);
    auto lease=w.AcquireDatabaseLease();auto pin=lease->CopyRecoverySeed(lease->Session());EXPECT_FALSE(w.resolveSigningKeyForScriptPubKey(util::hex(old.spk)));EXPECT_TRUE(lease->ResolveSigningKey(util::hex(old.spk),*pin));
}
TEST_F(HistoricalRpc, BuilderExactKeysAndAtomicWitnessPublication) {
    auto& w=service->get();auto a=w.resolveSigningKeyForScriptPubKey(util::hex(old.spk)),b=w.resolveSigningKeyForScriptPubKey(util::hex(modern.spk));ASSERT_TRUE(a&&b);
    dinero::TransactionBuilder builder(index.get());dinero::TransactionBuilder::BuildOptions options;options.candidate_utxos={old,modern};options.change_address=modern_address;
    const std::vector<dinero::TransactionBuilder::Recipient> recipients{{modern_address,150000}};
    std::map<std::string,dinero::SigningKey> keys{{old.GetOutpointString(),*a},{modern.GetOutpointString(),*b}};
    auto result=builder.BuildTransactionWithKeys(recipients,keys,options);ASSERT_TRUE(result.success)<<result.error;ASSERT_EQ(result.selected_utxos.size(),2u);verify(result.transaction,result.selected_utxos);
    EXPECT_EQ(result.required_private_keys.size(),2u);for(const auto& id:result.required_private_keys){EXPECT_TRUE(keys.count(id));EXPECT_NE(id,util::hex(a->secret));EXPECT_NE(id,util::hex(b->secret));}
    const auto last=result.selected_utxos.back().GetOutpointString();keys[last].secret.back()^=1;result=builder.BuildTransactionWithKeys(recipients,keys,options);EXPECT_FALSE(result.success);for(const auto& in:result.transaction.vin)EXPECT_TRUE(in.witness.empty());
    keys.clear();keys.emplace("historical label",*a);keys.emplace(modern.path,*b);EXPECT_FALSE(builder.BuildTransactionWithKeys(recipients,keys,options).success);
}
TEST_F(HistoricalRpc, ExistingWitnessAndMissingMetadataStayExplicit) {
    const std::vector<dinero::CanonicalWalletUTXO> coins{old,modern};auto p=params(coins);
    auto signed_result=rpc_context_wallet_signrawtransaction(ctx,p);ASSERT_TRUE(signed_result.isMember("hex"));
    p[0]=signed_result["hex"];auto repeated=rpc_context_wallet_signrawtransaction(ctx,p);
    ASSERT_TRUE(repeated.isMember("hex"));EXPECT_EQ(repeated["hex"].asString(),signed_result["hex"].asString());
    EXPECT_FALSE(repeated["complete"].asBool());
    service->get().lockWallet();auto locked_repeat=rpc_context_wallet_signrawtransaction(ctx,p);
    ASSERT_TRUE(locked_repeat.isMember("hex"));EXPECT_EQ(locked_repeat["hex"].asString(),signed_result["hex"].asString());
    EXPECT_FALSE(locked_repeat["complete"].asBool());service->get().unlockWallet("historical-rpc",0);
    p=params(coins);p[1].resize(1);auto missing=rpc_context_wallet_signrawtransaction(ctx,p);
    ASSERT_TRUE(missing.isMember("hex"));dinero::Transaction tx;
    ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(tx,missing["hex"].asString()));
    for(const auto& input:tx.vin)EXPECT_TRUE(input.witness.empty());EXPECT_FALSE(missing["complete"].asBool());
    auto* db=service->get().getCurrentDatabase();
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_READ && table && std::strcmp(table,"imported_keys")==0?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    auto denied=rpc_context_wallet_signrawtransaction(ctx,params({old}));sqlite3_set_authorizer(db,nullptr,nullptr);
    ASSERT_TRUE(denied.isMember("hex"));ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(tx,denied["hex"].asString()));
    EXPECT_TRUE(tx.vin[0].witness.empty());EXPECT_FALSE(denied["complete"].asBool());
    auto retry=rpc_context_wallet_signrawtransaction(ctx,params({old}));ASSERT_TRUE(retry.isMember("hex"));
    ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(tx,retry["hex"].asString()));verify(tx,{old});
}

class WalletOwnedSigning : public HistoricalRpc {
protected:
    static dinero::UnsignedTransaction unsigned_tx(const std::vector<dinero::CanonicalWalletUTXO>& coins) {
        dinero::UnsignedTransaction out;out.tx=transaction(coins);out.selected_utxos=coins;out.fee=1000;out.change_amount=0;out.signals_rbf=false;return out;
    }
    dinero::CanonicalWalletUTXO pq_coin() {
        registerV7PqWalletMethods();auto* handler=g_rpcRegistry.lookup("wallet.importp2mrseed");
        if(!handler)throw std::runtime_error("PQ adapter absent");
        din::Json p;p["seed_hex"]=std::string(64,'7');p["derivation_path"]="external:owned-signing";p["hrp"]="rdin";
        const auto result=(*handler)(ctx,p);
        if(result.isMember("error") && !result["error"].asString().empty())throw std::runtime_error(result.toStyledString());
        const auto script=dinero::TransactionBuilder::AddressToScriptPubKey(result["address"].asString());
        if(script.size()!=34 || script[0]!=0x53)throw std::runtime_error("PQ script absent");
        return coin(script,3);
    }
};
TEST_F(WalletOwnedSigning, ActualMixedOwnerSigningAndPqStore) {
    auto pq=pq_coin();auto& w=service->get();const auto identity=dinero::CaptureWalletSigningIdentity(w,"owner");
    const std::vector<dinero::CanonicalWalletUTXO> coins{old,modern,hd,pq};const auto input=unsigned_tx(coins);
    const auto before=input.tx.SerializeHex(true);const auto result=dinero::SignWalletTransaction(w,identity,input);
    ASSERT_TRUE(result.success)<<result.error;EXPECT_EQ(input.tx.SerializeHex(true),before);EXPECT_EQ(result.signed_tx.fee,input.fee);EXPECT_EQ(result.signed_tx.change_amount,input.change_amount);
    const auto& tx=result.signed_tx.tx;
    for(size_t i=0;i<coins.size();++i){
        dinero::consensus::ScriptExecutionContext c(&tx,i,coins[i].value.GetUna(),dinero::consensus::SCRIPT_VERIFY_WITNESS|dinero::consensus::SCRIPT_VERIFY_TAPROOT);
        for(const auto& u:coins){c.all_amounts.push_back(u.value.GetUna());c.all_scriptpubkeys.push_back(u.spk);c.all_confidential_flags.push_back(0);c.all_input_commitments.push_back({});}
        if(i<3){dinero::consensus::ScriptError error;EXPECT_TRUE(dinero::consensus::VerifyScript(dinero::consensus::Script(tx.vin[i].scriptSig),dinero::consensus::Script(coins[i].spk),tx.vin[i].witness,c,error));}
        else {const auto hash=dinero::consensus::SignatureHashTaproot(c,0,{});ASSERT_EQ(hash.size(),32u);std::array<uint8_t,32> message{};std::copy(hash.begin(),hash.end(),message.begin());ASSERT_EQ(tx.vin[i].witness.size(),1u);EXPECT_EQ(dinero::consensus::pq::VerifyP2MRSpend(pq.spk,tx.vin[i].witness[0],message,0),dinero::consensus::pq::P2MRVerifyError::Ok);message[0]^=1;EXPECT_NE(dinero::consensus::pq::VerifyP2MRSpend(pq.spk,tx.vin[i].witness[0],message,0),dinero::consensus::pq::P2MRVerifyError::Ok);}
    }
    const auto store=std::filesystem::path(w.GetV7P2MRStorePath());auto held=store;held+=".held";std::filesystem::rename(store,held);
    auto missing=dinero::SignWalletTransaction(w,identity,input);EXPECT_FALSE(missing.success);EXPECT_TRUE(missing.signed_tx.tx.vin.empty());EXPECT_FALSE(std::filesystem::exists(store));
    if(std::filesystem::exists(store))std::filesystem::remove(store);std::filesystem::rename(held,store);
    EXPECT_TRUE(dinero::SignWalletTransaction(w,identity,input).success);
}
TEST_F(WalletOwnedSigning, CapturedSessionPinAndCallerTransaction) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();const auto identity=dinero::CaptureWalletSigningIdentity(w,"owner");const auto input=unsigned_tx({old,modern});
    SigningHook hook{w};sqlite3_trace_v2(db,SQLITE_TRACE_STMT,SigningHook::trace,&hook);auto result=dinero::SignWalletTransaction(w,identity,input);sqlite3_trace_v2(db,0,nullptr,nullptr);
    ASSERT_TRUE(result.success)<<result.error;EXPECT_TRUE(hook.fired);EXPECT_TRUE(hook.lock_refused);EXPECT_TRUE(hook.selection_refused);verify(result.signed_tx.tx,input.selected_utxos);
    EXPECT_THROW(dinero::CaptureWalletSigningIdentity(w,"other"),std::runtime_error);
    sql(db,"BEGIN");EXPECT_FALSE(dinero::SignWalletTransaction(w,identity,input).success);EXPECT_FALSE(sqlite3_get_autocommit(db));sql(db,"ROLLBACK");
    w.lockWallet();EXPECT_FALSE(dinero::SignWalletTransaction(w,identity,input).success);w.unlockWallet("historical-rpc",0);
    w.open("owner");w.unlockWallet("historical-rpc",0);EXPECT_FALSE(dinero::SignWalletTransaction(w,identity,input).success);
    auto current=dinero::CaptureWalletSigningIdentity(w,"owner");EXPECT_TRUE(dinero::SignWalletTransaction(w,current,input).success);
    w.open("other");EXPECT_FALSE(dinero::SignWalletTransaction(w,current,input).success);
}
TEST_F(WalletOwnedSigning, CompleteOutpointsBeforeKeyReadsAndNoFallback) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();const auto identity=dinero::CaptureWalletSigningIdentity(w,"owner");
    for(int mode=0;mode<3;++mode){auto input=unsigned_tx({old,modern});if(mode==0)input.selected_utxos.pop_back();if(mode==1)input.selected_utxos[1].vout+=1;if(mode==2){input.selected_utxos[1]=old;input.tx.vin[1].prevout=input.tx.vin[0].prevout;}
        SigningHook hook{w};sqlite3_trace_v2(db,SQLITE_TRACE_STMT,SigningHook::trace,&hook);const auto result=dinero::SignWalletTransaction(w,identity,input);sqlite3_trace_v2(db,0,nullptr,nullptr);EXPECT_FALSE(result.success);EXPECT_TRUE(result.signed_tx.tx.vin.empty());EXPECT_FALSE(hook.fired);
    }
    auto invalid_fee=unsigned_tx({old,modern});invalid_fee.fee=0;
    auto invalid_result=dinero::SignWalletTransaction(w,identity,invalid_fee);EXPECT_FALSE(invalid_result.success);EXPECT_TRUE(invalid_result.signed_tx.tx.vin.empty());
    // A damaged durable owner must not fall back to another input's scalar or
    // a remembered HD label. Only synthetic fixture records are changed.
    sql(db,"UPDATE imported_keys SET private_key_enc='broken'");auto input=unsigned_tx({old,modern});input.selected_utxos[0].path=hd.path;
    const auto result=dinero::SignWalletTransaction(w,identity,input);EXPECT_FALSE(result.success);EXPECT_TRUE(result.signed_tx.tx.vin.empty());for(const auto& in:input.tx.vin)EXPECT_TRUE(in.witness.empty());
}

class WalletPendingPayment : public WalletOwnedSigning {
protected:
    void fund(const dinero::CanonicalWalletUTXO& c) {
        sql(service->get().getCurrentDatabase(),"INSERT INTO utxos(wallet_id,txid,vout,address,amount,script_pubkey,height,is_coinbase,is_mature,is_spent) VALUES(1,'"+c.GetTxIdHex()+"',"+std::to_string(c.vout)+",'fixture',"+std::to_string(c.value.GetUna())+",'"+util::hex(c.spk)+"',1,0,1,0)");
    }
    dinero::UnsignedTransaction payment() {
        auto result=unsigned_tx({old,modern});result.tx.vout[0].scriptPubKey=modern.spk;return result;
    }
    dinero::SignResult stage(const dinero::UnsignedTransaction& input) {
        auto& w=service->get();return dinero::SignAndStageWalletPayment(w,dinero::CaptureWalletSigningIdentity(w,"owner"),input,{modern_address,199000,""});
    }
    static int count(sqlite3* db,const char* table) {
        sqlite3_stmt* q=nullptr;const std::string text="SELECT count(*) FROM "+std::string(table);
        if(sqlite3_prepare_v2(db,text.c_str(),-1,&q,nullptr)!=SQLITE_OK)throw std::runtime_error("count prepare");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> owned(q,sqlite3_finalize);
        if(sqlite3_step(q)!=SQLITE_ROW)throw std::runtime_error("count read");const int n=sqlite3_column_int(q,0);
        if(sqlite3_step(q)!=SQLITE_DONE)throw std::runtime_error("count EOF");return n;
    }
    static std::string envelope(sqlite3* db) {
        sqlite3_stmt* q=nullptr;
        if(sqlite3_prepare_v2(db,"SELECT hex(pending_payment_owner) FROM wallet_meta WHERE id=1",-1,&q,nullptr)!=SQLITE_OK)throw std::runtime_error("envelope prepare");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> owned(q,sqlite3_finalize);
        if(sqlite3_step(q)!=SQLITE_ROW)throw std::runtime_error("envelope read");const auto* p=sqlite3_column_text(q,0);
        std::string result(reinterpret_cast<const char*>(p),sqlite3_column_bytes(q,0));
        if(sqlite3_step(q)!=SQLITE_DONE)throw std::runtime_error("envelope EOF");return result;
    }
};
TEST_F(WalletPendingPayment, SignedOriginHistoryReservationsAndReopen) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();fund(old);fund(modern);
    const auto input=payment();const auto result=stage(input);ASSERT_TRUE(result.success)<<result.error;verify(result.signed_tx.tx,{old,modern});
    auto records=w.getPendingPayments();ASSERT_EQ(records.size(),1u);const auto p=records[0];
    EXPECT_EQ(p.txid,result.signed_tx.tx.GetTxid().AsUint256().GetHex());EXPECT_EQ(p.signed_body,result.signed_tx.tx.Serialize(dinero::TxSerializationMode::WithWitness));
    EXPECT_EQ(p.intent.address,modern_address);EXPECT_EQ(p.intent.amount_una,199000u);EXPECT_EQ(p.fee_una,1000u);EXPECT_EQ(p.inputs.size(),2u);
    EXPECT_EQ(count(db,"transactions"),1);auto history=w.getTransactionHistory();ASSERT_EQ(history.size(),1u);EXPECT_EQ(history[0].category,"send");EXPECT_DOUBLE_EQ(history[0].amount,-0.002);
    EXPECT_TRUE(w.isUTXOLocked(old.GetTxIdHex(),old.vout));EXPECT_TRUE(w.isUTXOLocked(modern.GetTxIdHex(),modern.vout));EXPECT_EQ(w.getLockedUTXOs().size(),2u);EXPECT_DOUBLE_EQ(w.getLockedBalance(),0.002);
    EXPECT_FALSE(w.unlockUTXO(old.GetTxIdHex(),old.vout));EXPECT_EQ(w.unlockAllUTXOs(),0u);EXPECT_FALSE(w.abandonTransaction(p.txid));EXPECT_FALSE(w.getAbandonmentInfo(p.txid).success);
    auto listed=rpc_context_wallet_listpendingpayments(ctx,din::Json());ASSERT_FALSE(listed.isMember("error"))<<listed.toStyledString();ASSERT_EQ(listed["payments"].size(),1u);EXPECT_EQ(listed["payments"][0]["hex"].asString(),util::hex(p.signed_body));
    ctx.walletName="other";EXPECT_TRUE(rpc_context_wallet_listpendingpayments(ctx,din::Json()).isMember("error"));ctx.walletName="owner";
    const auto stored=envelope(db);auto duplicate=stage(input);EXPECT_FALSE(duplicate.success);EXPECT_TRUE(duplicate.signed_tx.tx.vin.empty());EXPECT_EQ(envelope(db),stored);EXPECT_EQ(count(db,"transactions"),1);
    // A stale selection for a distinct body still cannot obtain the same reservations.
    auto conflicting=input;conflicting.tx.lockTime=17;auto refused=stage(conflicting);EXPECT_FALSE(refused.success);EXPECT_TRUE(refused.signed_tx.tx.vin.empty());EXPECT_EQ(envelope(db),stored);
    w.lockWallet();EXPECT_THROW(w.getPendingPayments(),std::runtime_error);w.open("owner");w.unlockWallet("historical-rpc",0);
    records=w.getPendingPayments();ASSERT_EQ(records.size(),1u);EXPECT_EQ(records[0].signed_body,p.signed_body);EXPECT_TRUE(w.isUTXOLocked(old.GetTxIdHex(),old.vout));
    // Existing chain observations preserve the immutable origin; rollback leaves reservations intact.
    ASSERT_TRUE(w.confirmTransaction(p.txid,2));EXPECT_EQ(envelope(w.getCurrentDatabase()),stored);EXPECT_EQ(w.getPendingPayments()[0].signed_body,p.signed_body);
    auto second=input;second.selected_utxos[0].vout=7;second.selected_utxos[1].vout=8;
    second.tx.vin[0].prevout.vout=7;second.tx.vin[1].prevout.vout=8;
    fund(second.selected_utxos[0]);fund(second.selected_utxos[1]);auto next=stage(second);ASSERT_TRUE(next.success)<<next.error;
    auto both=w.getPendingPayments();ASSERT_EQ(both.size(),2u);EXPECT_EQ(both[0].signed_body,p.signed_body);EXPECT_EQ(both[1].signed_body,next.signed_tx.tx.Serialize(dinero::TxSerializationMode::WithWitness));
    w.open("owner");w.unlockWallet("historical-rpc",0);EXPECT_EQ(w.getPendingPayments().size(),2u);EXPECT_EQ(w.getLockedUTXOs().size(),4u);
}
TEST_F(WalletPendingPayment, FailedWritesCommitAndStaleSelectionPublishNothing) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();fund(old);fund(modern);const auto input=payment();
    const auto failure=[&]{auto r=stage(input);EXPECT_FALSE(r.success);EXPECT_TRUE(r.signed_tx.tx.vin.empty());EXPECT_TRUE(w.getPendingPayments().empty());EXPECT_EQ(count(db,"transactions"),0);};
    sql(db,"UPDATE utxos SET amount=amount-1 WHERE vout=1");failure();sql(db,"UPDATE utxos SET amount=amount+1 WHERE vout=1");
    sql(db,"UPDATE utxos SET is_spent=1 WHERE vout=1");failure();sql(db,"UPDATE utxos SET is_spent=0 WHERE vout=1");
    w.lockUTXO(old.GetTxIdHex(),old.vout);failure();w.unlockUTXO(old.GetTxIdHex(),old.vout);
    sql(db,"CREATE TRIGGER payment_history_failure BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT,'payment fixture'); END");failure();sql(db,"DROP TRIGGER payment_history_failure");
    struct Hook {bool writing=false;int commits=0;static int trace(unsigned,void* p,void* q,void*) {auto& h=*static_cast<Hook*>(p);const auto* s=sqlite3_sql(static_cast<sqlite3_stmt*>(q));if(s && std::strstr(s,"UPDATE wallet_meta SET pending_payment_owner"))h.writing=true;return 0;}static int commit(void* p){auto& h=*static_cast<Hook*>(p);if(h.writing){++h.commits;return 1;}return 0;}} hook;
    sqlite3_trace_v2(db,SQLITE_TRACE_STMT,Hook::trace,&hook);sqlite3_commit_hook(db,Hook::commit,&hook);auto r=stage(input);sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_trace_v2(db,0,nullptr,nullptr);
    EXPECT_FALSE(r.success);EXPECT_TRUE(r.signed_tx.tx.vin.empty());EXPECT_EQ(hook.commits,1);EXPECT_TRUE(w.getPendingPayments().empty());EXPECT_EQ(count(db,"transactions"),0);
    const auto identity=dinero::CaptureWalletSigningIdentity(w,"owner");
    sql(db,"BEGIN");r=dinero::SignAndStageWalletPayment(w,identity,input,{modern_address,199000,""});EXPECT_FALSE(r.success);EXPECT_FALSE(sqlite3_get_autocommit(db));sql(db,"ROLLBACK");
    r=stage(input);ASSERT_TRUE(r.success)<<r.error;EXPECT_EQ(count(db,"transactions"),1);
}
TEST_F(WalletPendingPayment, CorruptOwnerReadFailureAndIntentRefuse) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();fund(old);fund(modern);auto input=payment();
    auto wrong=input;wrong.tx.vout[0].value=dinero::AmountUna::Una(198999);auto bad=stage(wrong);EXPECT_FALSE(bad.success);EXPECT_TRUE(bad.signed_tx.tx.vin.empty());EXPECT_TRUE(w.getPendingPayments().empty());
    const auto result=stage(input);ASSERT_TRUE(result.success)<<result.error;const auto stored=envelope(db);
    for(const auto& mutation:{std::string("NULL"),std::string("X'00'"),std::string("'wrong SQL type'"),std::string("X'")+(stored.substr(0,2)=="00"?"01":"00")+stored.substr(2)+"'"}) {
        sql(db,"UPDATE wallet_meta SET pending_payment_owner="+mutation+" WHERE id=1");EXPECT_THROW(w.getPendingPayments(),std::runtime_error);
        EXPECT_THROW(w.isUTXOLocked(old.GetTxIdHex(),old.vout),std::runtime_error);
        w.lockWallet();EXPECT_THROW(w.unlockWallet("historical-rpc",0),std::runtime_error);EXPECT_TRUE(w.isWalletLocked());
        sql(db,"UPDATE wallet_meta SET pending_payment_owner=X'"+stored+"' WHERE id=1");w.unlockWallet("historical-rpc",0);
    }
    sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char* column,const char*,const char*){return op==SQLITE_READ && table && column && std::strcmp(table,"wallet_meta")==0 && std::strcmp(column,"pending_payment_owner")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(w.getPendingPayments(),std::runtime_error);auto denied=stage(input);EXPECT_FALSE(denied.success);EXPECT_TRUE(denied.signed_tx.tx.vin.empty());sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(envelope(db),stored);EXPECT_EQ(w.getPendingPayments().size(),1u);
    struct Interrupt {sqlite3* db;bool fired=false;static int trace(unsigned type,void* p,void* stmt,void*){auto& h=*static_cast<Interrupt*>(p);const auto* s=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));if(type==SQLITE_TRACE_ROW && !h.fired && s && std::strstr(s,"SELECT pending_payment_owner")){h.fired=true;sqlite3_interrupt(h.db);}return 0;}} interrupted{db};
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,Interrupt::trace,&interrupted);EXPECT_THROW(w.getPendingPayments(),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);EXPECT_TRUE(interrupted.fired);EXPECT_EQ(w.getPendingPayments().size(),1u);
}


class WalletReorgOrigin : public WalletPendingPayment {
protected:
    dinero::Block confirmed_payment() {
        fund(old);fund(modern);
        const auto signed_payment=stage(payment());
        if(!signed_payment.success)throw std::runtime_error(signed_payment.error);
        dinero::Block block;block.header.timestamp=1700000042;
        block.vtx.push_back(signed_payment.signed_tx.tx);
        service->get().onBlockConnected(block,2);
        return block;
    }
    static int scalar(sqlite3* db,const char* text) {
        sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(db,text,-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("reorg scalar prepare");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(raw,sqlite3_finalize);
        if(sqlite3_step(raw)!=SQLITE_ROW)throw std::runtime_error("reorg scalar row");
        int result=sqlite3_column_int(raw,0);
        if(sqlite3_step(raw)!=SQLITE_DONE)throw std::runtime_error("reorg scalar EOF");
        return result;
    }
    void assert_origin(const dinero::Block& block,const std::string& stored,bool confirmed) {
        auto& w=service->get();auto* db=w.getCurrentDatabase();
        EXPECT_EQ(envelope(db),stored);
        const auto payments=w.getPendingPayments();ASSERT_EQ(payments.size(),1u);
        EXPECT_EQ(payments[0].signed_body,block.vtx[0].Serialize(dinero::TxSerializationMode::WithWitness));
        const auto history=w.getTransactionHistory();ASSERT_EQ(history.size(),1u);
        EXPECT_EQ(history[0].txid,block.vtx[0].GetTxid().AsUint256().GetHex());
        EXPECT_EQ(history[0].category,"send");EXPECT_DOUBLE_EQ(history[0].amount,-0.002);
        EXPECT_EQ(history[0].address,modern_address);EXPECT_EQ(history[0].label,"");
        EXPECT_EQ(scalar(db,"SELECT height FROM transactions"),confirmed?2:0);
        EXPECT_EQ(history[0].confirmations,confirmed?1:0);
        EXPECT_TRUE(w.isUTXOLocked(old.GetTxIdHex(),old.vout));
        EXPECT_TRUE(w.isUTXOLocked(modern.GetTxIdHex(),modern.vout));
    }
};
TEST_F(WalletReorgOrigin, ActualDisconnectRetainsOriginAndReservationsAcrossReopen) {
    auto& w=service->get();const auto block=confirmed_payment();const auto stored=envelope(w.getCurrentDatabase());
    const auto before=w.getTransactionHistory();ASSERT_EQ(before.size(),1u);assert_origin(block,stored,true);
    ASSERT_EQ(scalar(w.getCurrentDatabase(),"SELECT count(*) FROM utxos WHERE is_spent=1"),2);
    w.onBlockDisconnected(block,2);assert_origin(block,stored,false);
    EXPECT_EQ(w.getTransactionHistory()[0].time,before[0].time);
    EXPECT_EQ(scalar(w.getCurrentDatabase(),"SELECT count(*) FROM utxos WHERE is_spent=1"),0);
    EXPECT_EQ(scalar(w.getCurrentDatabase(),"SELECT count(*) FROM utxos WHERE height=2"),0);
    EXPECT_EQ(w.getBlockchainHeight(),1u);
    w.open("owner");w.unlockWallet("historical-rpc",0);assert_origin(block,stored,false);
    // Actual listunspent keeps the restored outputs visible, explicitly reserved.
    w.setUTXOIndex(nullptr);w.setBlockchainHeight(3);
    const auto listed=rpc_context_wallet_listunspent(ctx,din::Json());
    ASSERT_TRUE(listed.isArray())<<listed.toStyledString();ASSERT_EQ(listed.size(),2u);
    for(const auto& item:listed){EXPECT_TRUE(item["locked"].asBool());EXPECT_FALSE(item["spendable"].asBool());}
    w.setUTXOIndex(index.get());w.onBlockConnected(block,2);assert_origin(block,stored,true);
    w.onBlockDisconnected(block,2);assert_origin(block,stored,false);
}
TEST_F(WalletReorgOrigin, DisconnectSqlAndCommitFailuresPreserveWholeGroup) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();const auto block=confirmed_payment();const auto stored=envelope(db);
    const int rows=count(db,"utxos");
    const auto unchanged=[&]{assert_origin(block,stored,true);EXPECT_EQ(count(db,"utxos"),rows);EXPECT_EQ(scalar(db,"SELECT count(*) FROM utxos WHERE is_spent=1"),2);EXPECT_EQ(w.getBlockchainHeight(),2u);};
    for(const auto* table:{"transactions","utxos"}) {
        sql(db,"CREATE TRIGGER refuse_history BEFORE UPDATE ON "+std::string(table)+" BEGIN SELECT RAISE(ABORT,'history rollback fixture'); END");
        EXPECT_THROW(w.onBlockDisconnected(block,2),std::runtime_error);sql(db,"DROP TRIGGER refuse_history");unchanged();
    }
    sqlite3_set_authorizer(db,[](void*,int op,const char* a,const char*,const char*,const char*) {return op==SQLITE_PRAGMA && a && std::strcmp(a,"table_info")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(w.onBlockDisconnected(block,2),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);unchanged();
    int commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<int*>(p);return 1;},&commits);
    EXPECT_THROW(w.onBlockDisconnected(block,2),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_EQ(commits,1);unchanged();
    sql(db,"BEGIN");EXPECT_THROW(w.onBlockDisconnected(block,2),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));sql(db,"ROLLBACK");unchanged();
    w.onBlockDisconnected(block,2);assert_origin(block,stored,false);
}
TEST_F(WalletReorgOrigin, StandaloneRewindPreservesLocalHistoryAndChecksTransaction) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();const auto block=confirmed_payment();const auto stored=envelope(db);
    ASSERT_TRUE(w.addTransaction(std::string(64,'a'),modern_address,0.001,"receive",false,"observed",1700000099,2));
    ASSERT_EQ(count(db,"transactions"),2);
    sql(db,"CREATE TRIGGER refuse_history BEFORE DELETE ON transactions BEGIN SELECT RAISE(ABORT,'history rollback fixture'); END");
    EXPECT_THROW(w.removeTransactionsAtHeight(2),std::runtime_error);sql(db,"DROP TRIGGER refuse_history");
    EXPECT_EQ(scalar(db,"SELECT count(*) FROM transactions WHERE height=2"),2);EXPECT_EQ(envelope(db),stored);
    sql(db,"BEGIN");EXPECT_THROW(w.removeTransactionsAtHeight(2),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));sql(db,"ROLLBACK");
    int commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<int*>(p);return 1;},&commits);
    EXPECT_THROW(w.removeTransactionsAtHeight(2),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_EQ(commits,1);
    EXPECT_EQ(scalar(db,"SELECT count(*) FROM transactions WHERE height=2"),2);
    ASSERT_TRUE(w.removeTransactionsAtHeight(2));assert_origin(block,stored,false);
    // This narrow history operation does not claim to undo UTXOs or publish height.
    EXPECT_EQ(scalar(db,"SELECT count(*) FROM utxos WHERE is_spent=1"),2);EXPECT_EQ(w.getBlockchainHeight(),2u);
    ASSERT_TRUE(w.removeTransactionsAtHeight(2));assert_origin(block,stored,false);
    w.open("owner");w.unlockWallet("historical-rpc",0);assert_origin(block,stored,false);
}

class WalletReservationBalance : public WalletPendingPayment {};
TEST_F(WalletReservationBalance, AuthenticatedReservationMaturityAndReopen) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();w.setBlockchainHeight(100);
    fund(old);fund(modern);fund(hd);
    auto immature=hd;immature.vout=3;fund(immature);
    auto unconfirmed=hd;unconfirmed.vout=4;fund(unconfirmed);
    auto spent=hd;spent.vout=5;fund(spent);
    sql(db,"UPDATE utxos SET is_coinbase=1,height=2 WHERE vout=3");
    sql(db,"UPDATE utxos SET height=0 WHERE vout=4");
    sql(db,"UPDATE utxos SET is_spent=1 WHERE vout=5");
    w.lockUTXO(hd.GetTxIdHex(),hd.vout);w.lockUTXO(immature.GetTxIdHex(),immature.vout);
    w.lockUTXO(unconfirmed.GetTxIdHex(),unconfirmed.vout);w.lockUTXO(spent.GetTxIdHex(),spent.vout);
    ASSERT_TRUE(stage(payment()).success);
    // A manual lock overlapping a retained reservation must not double count.
    w.lockUTXO(old.GetTxIdHex(),old.vout);
    const auto stored=envelope(db);const auto changes=sqlite3_total_changes(db);
    auto summary=w.getBalanceSummary("owner");
    EXPECT_EQ(summary.reservations,dinero::WalletManager::ReservationStatus::Authenticated);
    EXPECT_DOUBLE_EQ(summary.balance.confirmed,0.003);EXPECT_DOUBLE_EQ(summary.balance.unconfirmed,0.001);
    EXPECT_DOUBLE_EQ(summary.balance.immature,0.001);EXPECT_DOUBLE_EQ(summary.balance.total,0.005);
    EXPECT_EQ(summary.balance.utxo_count,5);EXPECT_EQ(summary.balance.immature_utxo_count,1);
    ASSERT_TRUE(summary.locked);EXPECT_DOUBLE_EQ(*summary.locked,0.005);
    ASSERT_TRUE(summary.available_confirmed);EXPECT_DOUBLE_EQ(*summary.available_confirmed,0);
    ASSERT_TRUE(summary.unavailable_confirmed_and_immature);EXPECT_DOUBLE_EQ(*summary.unavailable_confirmed_and_immature,0.004);
    EXPECT_DOUBLE_EQ(summary.available_by_script.at(util::hex(hd.spk)),0);
    EXPECT_EQ(sqlite3_total_changes(db),changes);EXPECT_EQ(envelope(db),stored);
    EXPECT_THROW(w.getBalanceSummary("other"),std::runtime_error);
    EXPECT_EQ(w.unlockAllUTXOs(),5u);
    summary=w.getBalanceSummary();EXPECT_DOUBLE_EQ(*summary.locked,0.002);EXPECT_DOUBLE_EQ(*summary.available_confirmed,0.001);
    w.open("owner");w.unlockWallet("historical-rpc",0);summary=w.getBalanceSummary("owner");
    EXPECT_DOUBLE_EQ(*summary.locked,0.002);EXPECT_DOUBLE_EQ(*summary.available_confirmed,0.001);EXPECT_EQ(envelope(w.getCurrentDatabase()),stored);
}
TEST_F(WalletReservationBalance, LockedRpcAmountsAreExplicitlyUnavailable) {
    auto& w=service->get();w.setBlockchainHeight(100);fund(old);fund(modern);fund(hd);
    ASSERT_TRUE(stage(payment()).success);const auto stored=envelope(w.getCurrentDatabase());w.lockWallet();
    const auto summary=w.getBalanceSummary("owner");EXPECT_EQ(summary.reservations,dinero::WalletManager::ReservationStatus::UnlockRequired);
    EXPECT_DOUBLE_EQ(summary.balance.confirmed,0.003);EXPECT_FALSE(summary.locked);EXPECT_FALSE(summary.available_confirmed);
    EXPECT_FALSE(summary.unavailable_confirmed_and_immature);EXPECT_TRUE(summary.available_by_script.empty());
    const auto balance=rpc_context_wallet_getbalance(ctx,din::Json());ASSERT_FALSE(balance.isMember("error"))<<balance.toStyledString();
    EXPECT_DOUBLE_EQ(balance["confirmed"].asDouble(),0.003);EXPECT_TRUE(balance["locked"].isNull());EXPECT_TRUE(balance["spendable"].isNull());
    EXPECT_EQ(balance["reservation_status"].asString(),"unlock_required");EXPECT_TRUE(balance["breakdown"]["unspendable"].isNull());
    const auto info=rpc_context_wallet_getwalletinfo(ctx,din::Json());ASSERT_FALSE(info.isMember("error"))<<info.toStyledString();
    EXPECT_DOUBLE_EQ(info["balance"].asDouble(),0.003);EXPECT_TRUE(info["locked_balance"].isNull());EXPECT_TRUE(info["spendable_balance"].isNull());
    EXPECT_EQ(info["reservation_status"].asString(),"unlock_required");
    const auto snapshot=rpc_context_wallet_snapshot(ctx,din::Json());ASSERT_FALSE(snapshot.isMember("error"))<<snapshot.toStyledString();
    EXPECT_DOUBLE_EQ(snapshot["balances"]["confirmed"].asDouble(),0.003);EXPECT_TRUE(snapshot["balances"]["locked"].isNull());
    EXPECT_TRUE(snapshot["balances"]["spendable"].isNull());EXPECT_EQ(snapshot["balances"]["reservation_status"].asString(),"unlock_required");
    ctx.walletName="other";EXPECT_TRUE(rpc_context_wallet_getbalance(ctx,din::Json()).isMember("error"));
    EXPECT_TRUE(rpc_context_wallet_getwalletinfo(ctx,din::Json()).isMember("error"));EXPECT_TRUE(rpc_context_wallet_snapshot(ctx,din::Json()).isMember("error"));ctx.walletName="owner";
    w.unlockWallet("historical-rpc",0);const auto unlocked=rpc_context_wallet_getbalance(ctx,din::Json());ASSERT_FALSE(unlocked.isMember("error"));
    EXPECT_EQ(unlocked["reservation_status"].asString(),"authenticated");EXPECT_DOUBLE_EQ(unlocked["spendable"].asDouble(),0.001);
    EXPECT_DOUBLE_EQ(unlocked["locked"].asDouble(),0.002);EXPECT_EQ(envelope(w.getCurrentDatabase()),stored);
}
TEST_F(WalletReservationBalance, UntrackedAndIncompleteReadsNeverClaimAuthentication) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();w.setBlockchainHeight(100);fund(old);fund(modern);
    w.lockUTXO(old.GetTxIdHex(),old.vout);w.lockWallet();
    auto summary=w.getBalanceSummary();EXPECT_EQ(summary.reservations,dinero::WalletManager::ReservationStatus::Untracked);
    EXPECT_DOUBLE_EQ(*summary.locked,0.001);EXPECT_DOUBLE_EQ(*summary.available_confirmed,0.001);
    sql(db,"UPDATE utxos SET amount='bad' WHERE vout=1");EXPECT_THROW(w.getBalanceSummary(),std::runtime_error);
    EXPECT_TRUE(rpc_context_wallet_getbalance(ctx,din::Json()).isMember("error"));sql(db,"UPDATE utxos SET amount=100000 WHERE vout=1");
    sql(db,"UPDATE utxos SET txid=upper(substr(txid,1,63)||'a') WHERE vout=1");
    EXPECT_THROW(w.getBalanceSummary(),std::runtime_error);
    sql(db,"UPDATE utxos SET txid='"+old.GetTxIdHex()+"' WHERE vout=1");
    sql(db,"BEGIN");EXPECT_THROW(w.getBalanceSummary(),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));sql(db,"ROLLBACK");
    struct Interrupt {sqlite3* db;bool fired=false;static int trace(unsigned type,void* p,void* stmt,void*) {auto& h=*static_cast<Interrupt*>(p);const auto* s=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));if(type==SQLITE_TRACE_ROW && !h.fired && s && std::strstr(s,"SELECT txid,vout,amount,height,is_coinbase,is_spent")){h.fired=true;sqlite3_interrupt(h.db);}return 0;}} interrupted{db};
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,Interrupt::trace,&interrupted);EXPECT_THROW(w.getBalanceSummary(),std::runtime_error);sqlite3_trace_v2(db,0,nullptr,nullptr);EXPECT_TRUE(interrupted.fired);
    sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char*,const char*,const char*) {return op==SQLITE_READ && table && std::strcmp(table,"utxos")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(w.getBalanceSummary(),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    w.unlockWallet("historical-rpc",0);w.unlockAllUTXOs();ASSERT_TRUE(stage(payment()).success);const auto stored=envelope(db);
    sql(db,"UPDATE wallet_meta SET pending_payment_owner=X'00'");EXPECT_THROW(w.getBalanceSummary(),std::runtime_error);
    const auto failed=rpc_context_wallet_getbalance(ctx,din::Json());EXPECT_TRUE(failed.isMember("error"));EXPECT_FALSE(failed.isMember("confirmed"));
    sql(db,"UPDATE wallet_meta SET pending_payment_owner=X'"+stored+"'");summary=w.getBalanceSummary();EXPECT_EQ(summary.reservations,dinero::WalletManager::ReservationStatus::Authenticated);EXPECT_DOUBLE_EQ(*summary.locked,0.002);
}

class WalletBatchPayment : public WalletPendingPayment {
protected:
    dinero::PendingPaymentIntent batch_intent() {
        return {modern_address,60000,"",{{service->get().getNewAddress(),139000}}};
    }
    dinero::UnsignedTransaction batch(const dinero::PendingPaymentIntent& intent,uint32_t offset=0) {
        auto coins=std::vector<dinero::CanonicalWalletUTXO>{old,modern};
        for(auto& c:coins)c.vout+=offset;
        auto input=unsigned_tx(coins);input.tx.vout.clear();
        dinero::TxOutput first;first.value=dinero::AmountUna::Una(intent.amount_una);first.scriptPubKey=dinero::TransactionBuilder::AddressToScriptPubKey(intent.address);input.tx.vout.push_back(first);
        for(const auto& recipient:intent.additional_recipients){dinero::TxOutput out;out.value=dinero::AmountUna::Una(recipient.amount_una);out.scriptPubKey=dinero::TransactionBuilder::AddressToScriptPubKey(recipient.address);input.tx.vout.push_back(out);}
        return input;
    }
    dinero::SignResult retain(const dinero::UnsignedTransaction& input,const dinero::PendingPaymentIntent& intent) {
        auto& w=service->get();return dinero::SignAndStageWalletPayment(w,dinero::CaptureWalletSigningIdentity(w,"owner"),input,intent);
    }
};
TEST_F(WalletBatchPayment, MixedLegacyBatchAndReopen) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();fund(old);fund(modern);
    const auto single=stage(payment());ASSERT_TRUE(single.success)<<single.error;
    const auto original=w.getPendingPayments().at(0);const auto initial=envelope(db);
    const auto intent=batch_intent();auto input=batch(intent,7);for(const auto& c:input.selected_utxos)fund(c);
    const auto result=retain(input,intent);ASSERT_TRUE(result.success)<<result.error;verify(result.signed_tx.tx,input.selected_utxos);
    auto records=w.getPendingPayments();ASSERT_EQ(records.size(),2u);EXPECT_EQ(records[0].signed_body,original.signed_body);EXPECT_EQ(records[0].created_at,original.created_at);EXPECT_TRUE(records[0].intent.additional_recipients.empty());
    ASSERT_EQ(records[1].intent.additional_recipients.size(),1u);EXPECT_EQ(records[1].intent.additional_recipients[0].address,intent.additional_recipients[0].address);EXPECT_EQ(records[1].intent.additional_recipients[0].amount_una,139000u);EXPECT_NE(envelope(db),initial);
    auto listing=rpc_context_wallet_listpendingpayments(ctx,din::Json());ASSERT_FALSE(listing.isMember("error"))<<listing.toStyledString();ASSERT_EQ(listing["payments"].size(),2u);EXPECT_EQ(listing["payments"][0]["recipients"].size(),1u);EXPECT_EQ(listing["payments"][1]["recipients"].size(),2u);EXPECT_EQ(listing["payments"][1]["total_amount_una"].asUInt64(),199000u);
    auto history=w.getTransactionHistory();ASSERT_EQ(history.size(),2u);for(const auto& row:history){EXPECT_EQ(row.category,"send");EXPECT_DOUBLE_EQ(row.amount,-0.002);EXPECT_EQ(row.address,modern_address);}
    const auto saved=envelope(db);w.open("owner");w.unlockWallet("historical-rpc",0);records=w.getPendingPayments();ASSERT_EQ(records.size(),2u);EXPECT_EQ(records[0].signed_body,original.signed_body);EXPECT_EQ(records[1].signed_body,result.signed_tx.tx.Serialize(dinero::TxSerializationMode::WithWitness));EXPECT_EQ(w.getLockedUTXOs().size(),4u);EXPECT_EQ(envelope(w.getCurrentDatabase()),saved);
    // Equal recipients still require separate actual outputs, not one reused match.
    auto repeated=intent;repeated.additional_recipients={{modern_address,60000}};auto duplicate=batch(repeated,20);duplicate.tx.vout[0].value=dinero::AmountUna::Una(139000);
    for(const auto& c:duplicate.selected_utxos)fund(c);auto refused=retain(duplicate,repeated);EXPECT_FALSE(refused.success);EXPECT_EQ(envelope(w.getCurrentDatabase()),saved);
}
TEST_F(WalletBatchPayment, OutputIntentAndCommitRefuseWithoutPublication) {
    auto& w=service->get();auto* db=w.getCurrentDatabase();fund(old);fund(modern);const auto intent=batch_intent();const auto input=batch(intent);
    auto wrong=intent;wrong.additional_recipients[0].amount_una++;auto result=retain(input,wrong);EXPECT_FALSE(result.success);EXPECT_TRUE(result.signed_tx.tx.vin.empty());EXPECT_EQ(count(db,"transactions"),0);EXPECT_TRUE(w.getPendingPayments().empty());
    sqlite3_commit_hook(db,[](void*){return 1;},nullptr);result=retain(input,intent);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_FALSE(result.success);EXPECT_TRUE(result.signed_tx.tx.vin.empty());EXPECT_EQ(count(db,"transactions"),0);EXPECT_TRUE(w.getPendingPayments().empty());EXPECT_TRUE(w.getLockedUTXOs().empty());
    result=retain(input,intent);ASSERT_TRUE(result.success)<<result.error;EXPECT_EQ(w.getPendingPayments().size(),1u);
}
class WalletBatchRpc : public WalletPendingPayment {
protected:
    struct Ingress final:dinero::ITxIngress {
        std::function<std::optional<dinero::TxAcceptResult>(const dinero::Transaction&)> test;
        std::function<dinero::TxAcceptResult(const dinero::Transaction&)> submit;
        int tests=0,submits=0;
        std::optional<dinero::TxAcceptResult> Test(const dinero::Transaction& tx,dinero::TxOrigin origin) override {EXPECT_EQ(origin,dinero::TxOrigin::WALLET);++tests;return test?test(tx):std::nullopt;}
        dinero::TxAcceptResult Submit(const dinero::Transaction& tx,dinero::TxOrigin origin) override {EXPECT_EQ(origin,dinero::TxOrigin::WALLET);++submits;return submit(tx);}
        bool HasTransaction(const dinero::uint256&)const override{return false;}
        std::shared_ptr<dinero::Transaction> GetTransaction(const dinero::uint256&)const override{return {};}
    };
    std::shared_ptr<dinero::ChainstateService> chain;
    std::shared_ptr<Ingress> ingress;
    void SetUp() override {
        WalletPendingPayment::SetUp();if(HasFatalFailure())return;
        chain=std::make_shared<dinero::ChainstateService>();dinero::WalletBatchPaymentTestAccess::InstallIndex(*chain,std::move(index));daemon.chainstate=chain;
        ingress=std::make_shared<Ingress>();daemon.tx_ingress=ingress.get();
        auto& w=service->get();w.setBlockchainHeight(100);fund(hd);
        ASSERT_TRUE(chain->utxoIndex()->AddUTXO(dinero::WalletUTXO(dinero::TxId(hd.txid),hd.vout,hd.value,hd.spk,hd.path,hd.height)));
    }
    void TearDown() override {service->get().setUTXOIndex(nullptr);daemon.tx_ingress=nullptr;ingress.reset();daemon.chainstate.reset();chain.reset();WalletPendingPayment::TearDown();}
    din::Json request() {
        din::Json result(Json::arrayValue),recipients;recipients[modern_address]="0.00020000";recipients[service->get().getNewAddress()]="0.00030000";result.append(recipients);result.append(1.0);return result;
    }
    void before_preflight(const dinero::Transaction& tx) {
        auto& w=service->get();EXPECT_TRUE(sqlite3_get_autocommit(w.getCurrentDatabase()));EXPECT_TRUE(w.getPendingPayments().empty());EXPECT_EQ(count(w.getCurrentDatabase(),"transactions"),0);verify(tx,{hd});
    }
    void at_submission(const dinero::Transaction& tx) {
        auto& w=service->get();EXPECT_TRUE(sqlite3_get_autocommit(w.getCurrentDatabase()));auto records=w.getPendingPayments();ASSERT_EQ(records.size(),1u);EXPECT_EQ(records[0].signed_body,tx.Serialize(dinero::TxSerializationMode::WithWitness));ASSERT_EQ(records[0].intent.additional_recipients.size(),1u);EXPECT_EQ(records[0].intent.amount_una+records[0].intent.additional_recipients[0].amount_una,50000u);EXPECT_EQ(count(w.getCurrentDatabase(),"transactions"),1);EXPECT_TRUE(w.isUTXOLocked(hd.GetTxIdHex(),hd.vout));verify(tx,{hd});
    }
};
TEST_F(WalletBatchRpc, PreflightThenRetainBeforeOneRejectedSubmission) {
    std::vector<std::vector<uint8_t>> candidates;
    ingress->test=[&](const dinero::Transaction& tx){before_preflight(tx);candidates.push_back(tx.Serialize(dinero::TxSerializationMode::WithWitness));return ingress->tests==1?dinero::TxAcceptResult::Rejected(dinero::TxRejectCode::INSUFFICIENT_FEE,"fixture preflight fee"):dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    ingress->submit=[&](const dinero::Transaction& tx){at_submission(tx);return dinero::TxAcceptResult::Rejected(dinero::TxRejectCode::INSUFFICIENT_FEE,"fixture policy changed after preflight");};
    const auto result=rpc_context_wallet_sendmany(ctx,request());ASSERT_TRUE(result.isMember("error"))<<result.toStyledString();EXPECT_TRUE(result["payment_retained"].asBool());EXPECT_EQ(ingress->tests,2);EXPECT_EQ(ingress->submits,1);ASSERT_EQ(candidates.size(),2u);EXPECT_NE(candidates[0],candidates[1]);
    auto& w=service->get();const auto records=w.getPendingPayments();ASSERT_EQ(records.size(),1u);dinero::Transaction preflight,retained;ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(preflight,util::hex(candidates[1])));ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(retained,util::hex(records[0].signed_body)));EXPECT_EQ(retained.Serialize(dinero::TxSerializationMode::WithoutWitness),preflight.Serialize(dinero::TxSerializationMode::WithoutWitness));EXPECT_EQ(records[0].txid,result["txid"].asString());w.open("owner");w.unlockWallet("historical-rpc",0);EXPECT_EQ(w.getPendingPayments()[0].signed_body,records[0].signed_body);
}
TEST_F(WalletBatchRpc, UnavailablePreflightThenSubmissionExceptionRetains) {
    const auto p=request();auto result=rpc_context_wallet_sendmany(ctx,p);EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("payment_retained"));EXPECT_EQ(ingress->submits,0);EXPECT_TRUE(service->get().getPendingPayments().empty());
    ingress->test=[&](const dinero::Transaction& tx){before_preflight(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    ingress->submit=[&](const dinero::Transaction& tx)->dinero::TxAcceptResult{at_submission(tx);throw std::runtime_error("fixture submission result unavailable");};
    result=rpc_context_wallet_sendmany(ctx,p);EXPECT_TRUE(result.isMember("error"));EXPECT_TRUE(result["payment_retained"].asBool());EXPECT_EQ(ingress->submits,1);auto& w=service->get();auto records=w.getPendingPayments();ASSERT_EQ(records.size(),1u);EXPECT_EQ(records[0].txid,result["txid"].asString());EXPECT_FALSE(w.unlockUTXO(hd.GetTxIdHex(),hd.vout));
}
TEST_F(WalletBatchRpc, SuccessfulSubmissionAndInvalidInputBeforeEffects) {
    ingress->test=[&](const dinero::Transaction& tx){before_preflight(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    ingress->submit=[&](const dinero::Transaction& tx){at_submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    auto p=request();auto malformed=p;malformed[0][modern_address]="not-an-amount";auto result=rpc_context_wallet_sendmany(ctx,malformed);EXPECT_TRUE(result.isMember("error"));EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);EXPECT_TRUE(service->get().getPendingPayments().empty());
    ctx.walletName="other";result=rpc_context_wallet_sendmany(ctx,p);EXPECT_TRUE(result.isMember("error"));EXPECT_EQ(ingress->tests,0);ctx.walletName="owner";
    result=rpc_context_wallet_sendmany(ctx,p);EXPECT_FALSE(result.isMember("error"))<<result.toStyledString();EXPECT_TRUE(result["payment_retained"].asBool());EXPECT_EQ(result["recipients"].asInt(),2);EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);EXPECT_EQ(service->get().getPendingPayments().size(),1u);
}
#include "wallet_request_owner_checks.h"
#include "wallet_request_dispatch_checks.h"
#include "vault_retained_withdrawal_checks.h"
#include "vault_dispatch_lifetime_checks.h"
#include "vault_payment_binding_checks.h"

}
#include "vault_reservation_metrics_checks.h"

#include "wallet_pool_request_checks.h"

#include "wallet_pool_origin_checks.h"

#include "pool_payment_attempt_checks.h"

#include "pool_payment_eligibility_checks.h"

#include "pool_maintenance_source_checks.h"

#include "pool_orphan_retention_checks.h"

#include "pool_maintenance_worker_checks.h"
