#pragma once
#include "wallet/bip32_deriver.h"
#include "crypto/hash.h"
#include "external/bech32/bech32.hpp"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
struct ShieldBip84Fixture : ShieldReservationFixture {
    static constexpr const char* RecordedPath="m/84'/1448'/0'/0/7";
    std::string bip84_address;
    std::vector<uint8_t> bip84_script,public_key;
    struct Row {
        sqlite3_stmt* value=nullptr;
        Row(sqlite3* db,const char* sql){
            if(sqlite3_prepare_v2(db,sql,-1,&value,nullptr)!=SQLITE_OK){
                const auto error=std::string(sqlite3_errmsg(db));sqlite3_finalize(value);value=nullptr;
                throw std::runtime_error("fixture SQL prepare: "+error+"; statement: "+sql);
            }
        }
        ~Row(){sqlite3_finalize(value);}
        void Text(int i,const std::string& s){Need(sqlite3_bind_text(value,i,s.data(),int(s.size()),SQLITE_TRANSIENT)==SQLITE_OK);}
        void Blob(int i,const std::vector<uint8_t>& s){Need(sqlite3_bind_blob(value,i,s.data(),int(s.size()),SQLITE_TRANSIENT)==SQLITE_OK);}
        void Done(){Need(sqlite3_step(value)==SQLITE_DONE);}
    };
    ShieldBip84Fixture() {
        {
            auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
            auto seed=lease->CopyRecoverySeed(Selected().session);
            BIP32Deriver derive(seed->Bytes().data(),seed->Bytes().size());
            derive.deriveHardened(84);derive.deriveHardened(1448);derive.deriveHardened(0);
            derive.deriveNormal(0);derive.deriveNormal(7);
            const auto encoded=derive.getCompressedPubkey();public_key.assign(encoded.begin(),encoded.end());
            const auto hash=din::crypto::HASH160(public_key);const std::vector<uint8_t> program(hash.begin(),hash.end());
            bip84_address=bech32::Encode("rdin",0,program,bech32::Encoding::BECH32);
            bip84_script={0,20};bip84_script.insert(bip84_script.end(),program.begin(),program.end());
            // Reconstruct an actual historical BIP84 issuance tuple from this
            // wallet's seed. The path describes this key; it is not an import
            // relabel, nor permission to recreate a missing production owner.
            auto* db=lease->Database();InventoryTestTransaction tx(db);
            Row address_row(db,"INSERT INTO addresses(wallet_id,account,change,idx,address,pubkey,label,type,script_pubkey) "
                "SELECT id,0,0,7,?,?,'historical BIP84 fixture','p2wpkh',? FROM wallet_meta WHERE name='canonical-recovery'");
            address_row.Text(1,bip84_address);address_row.Blob(2,public_key);address_row.Text(3,util::hex(bip84_script));address_row.Done();
            Need(sqlite3_changes(db)==1);
            Row path_row(db,"INSERT INTO address_derivation_paths(address,derivation_path,script_pubkey,account,change,address_index) VALUES(?,?,?,0,0,7)");
            path_row.Text(1,bip84_address);path_row.Text(2,RecordedPath);path_row.Text(3,util::hex(bip84_script));path_row.Done();
            Row watch_row(db,"INSERT INTO watch_scripts(script_pubkey,path,is_change) VALUES(?,?,0)");
            watch_row.Blob(1,bip84_script);watch_row.Text(2,RecordedPath);watch_row.Done();tx.Commit();
        }
        Need(wallet->EnsureRuntimeWalletBindings());
        Need(wallet->RecoverActiveWalletFromCanonicalSource()==Recovery::AppliedPrefix);
        // Fund that exact script with an ordinary signed wallet payment and
        // retain its real intent/history before actual admission and mining.
        auto transfer=Input();transfer.tx.vout.clear();
        transfer.tx.vout.emplace_back(AmountUna::Una(40000),bip84_script);
        transfer.tx.vout.emplace_back(AmountUna::Una(59000),script);
        transfer.change_amount=59000;PendingPaymentIntent intent{bip84_address,40000,"BIP84 shield funding"};
        Transaction funded;
        {
            auto use=WalletService::AcquireWalletUse(wallet);auto source=ChainstateService::AcquireWalletIndexUse(f.service);
            const auto signed_payment=f.service->signAndStageWalletPayment(use->Wallet(),Selected(),transfer,intent);
            if(!signed_payment.success)throw std::runtime_error(signed_payment.error);
            funded=signed_payment.signed_tx.tx;
        }
        const auto block=Mine(MempoolTransaction(funded));Need(block&&block->Context()->height==103);
        Need(wallet->RecoverActiveWalletFromCanonicalSource()==Recovery::AppliedPrefix);
        funding.txid=funded.GetTxid().AsUint256();funding.vout=0;funding.value=AmountUna::Una(40000);
        funding.height=103;funding.spk=bip84_script;funding.path=RecordedPath;
        const auto inclusion=f.service->getCanonicalOutputInclusion(funding.txid,0,103);
        Need(inclusion.ok()&&inclusion->MatchesTransparent(40000,bip84_script));
        // No addUTXO for this output: recovery must have discovered it.
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        Row coin(lease->Database(),"SELECT amount,height,is_coinbase,is_spent,script_pubkey FROM utxos WHERE txid=? AND vout=0");
        coin.Text(1,funding.GetTxIdHex());Need(sqlite3_step(coin.value)==SQLITE_ROW);
        for(int col=0;col<4;++col)Need(sqlite3_column_type(coin.value,col)==SQLITE_INTEGER);
        Need(sqlite3_column_int64(coin.value,0)==40000&&sqlite3_column_int64(coin.value,1)==103&&
             sqlite3_column_int64(coin.value,2)==0&&sqlite3_column_int64(coin.value,3)==0);
        const auto hex=util::hex(bip84_script);Need(sqlite3_column_type(coin.value,4)==SQLITE_TEXT&&
            sqlite3_column_bytes(coin.value,4)==int(hex.size())&&sqlite3_column_blob(coin.value,4)&&
            std::memcmp(sqlite3_column_blob(coin.value,4),hex.data(),hex.size())==0);coin.Done();
    }
    std::string KeyMetadata() {
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        Row row(lease->Database(),"SELECT a.*,p.*,w.* FROM addresses a JOIN address_derivation_paths p ON p.address=a.address "
            "JOIN watch_scripts w ON w.script_pubkey=? WHERE a.address=?");
        row.Blob(1,bip84_script);row.Text(2,bip84_address);Need(sqlite3_step(row.value)==SQLITE_ROW);
        std::string bytes;
        for(int col=0;col<sqlite3_column_count(row.value);++col){
            bytes+=std::to_string(sqlite3_column_type(row.value,col))+":";
            const auto* data=static_cast<const char*>(sqlite3_column_blob(row.value,col));const int n=sqlite3_column_bytes(row.value,col);
            Need(n>=0&&(data||n==0));bytes+=std::to_string(n)+":";if(n)bytes.append(data,n);
        }
        row.Done();return bytes;
    }
    auto Outputs(){return std::vector<orchard::TransparentOutput>{{10000,bip84_script}};}
    auto QueueBip84(wallet::OrchardProofJobs& jobs) {
        const auto revision=Account(3).revision;const auto inputs=Inputs();const auto payments=Payments();const auto outputs=Outputs();
        auto use=WalletService::AcquireWalletUse(wallet);auto source=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->queueRuntimeWalletShield(use->Wallet(),Selected().session,3,revision,orchard::Hash{81},inputs,payments,outputs,10000,jobs);
    }
    auto FinishBip84(wallet::OrchardProofJobs& jobs) {
        const auto inputs=Inputs();const auto payments=Payments();const auto outputs=Outputs();
        auto use=WalletService::AcquireWalletUse(wallet);auto source=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->finalizeRuntimeWalletShield(use->Wallet(),Selected().session,3,orchard::Hash{81},inputs,payments,outputs,10000,jobs);
    }
};
}
TEST(WalletShieldBip84, RecordedSeedPathSignsMinesAndRecoversWithoutRelabel) {
    ShieldBip84Fixture f;const auto metadata=f.KeyMetadata();std::vector<uint8_t> body;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
        const auto queued=f.QueueBip84(jobs);ASSERT_TRUE(queued);ASSERT_TRUE(queued->enqueued);
        ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
        body=f.FinishBip84(jobs);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
    }
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);
    ASSERT_EQ(envelope.Inputs().size(),1u);const auto& witness=envelope.Inputs()[0].witness;
    ASSERT_EQ(witness.size(),2u);EXPECT_EQ(witness[1],f.public_key);
    ASSERT_GE(witness[0].size(),9u);ASSERT_LE(witness[0].size(),73u);EXPECT_EQ(witness[0].back(),1u);
    std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> secp(
        secp256k1_context_create(SECP256K1_CONTEXT_VERIFY),secp256k1_context_destroy);
    ASSERT_TRUE(secp);secp256k1_ecdsa_signature signature{};
    ASSERT_EQ(secp256k1_ecdsa_signature_parse_der(secp.get(),&signature,witness[0].data(),witness[0].size()-1),1);
    EXPECT_EQ(secp256k1_ecdsa_signature_normalize(secp.get(),nullptr,&signature),0);
    EXPECT_EQ(f.KeyMetadata(),metadata);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    wallet::OrchardProofJobs absent;EXPECT_EQ(f.FinishBip84(absent),body);EXPECT_EQ(f.KeyMetadata(),metadata);
    const auto block=f.Mine(MempoolTransaction::FromOrchard(envelope));ASSERT_TRUE(block);EXPECT_EQ(block->Context()->height,104u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    EXPECT_EQ(f.KeyMetadata(),metadata);
}
} // namespace dinero
#endif
