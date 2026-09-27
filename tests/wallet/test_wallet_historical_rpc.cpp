#include <gtest/gtest.h>
#include "common/test_logger.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/config_service.h"
#include "daemon/services/wallet_service.h"
#include "rpc/rpc_registry.h"
#include "wallet/transaction_builder.h"
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

}
