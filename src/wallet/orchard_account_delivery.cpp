#include "wallet/orchard_ownership_inventory.h"
#include "wallet/orchard_account_catalog.h"
#include "wallet/orchard_account_delivery.h"
#include <ctime>
#include "wallet/orchard_proof_jobs.h"
#include "wallet/runtime_account_replay.h"
#include "wallet/orchard_operation_archive.h"
#include <algorithm>
#include <map>
#include <set>
#include "wallet/wallet_manager.h"
#include "wallet/wallet_transaction_signer.h"
#include "util/hex.h"
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdexcept>
#include <type_traits>
#include "crypto/hash.h"
#include "crypto/tagged_hash.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
namespace dinero::wallet {
namespace {
void Check(bool v) { if(!v) throw std::runtime_error("Orchard account delivery ownership or state mismatch"); }
void CheckRequest(bool value, OrchardRequestError::Code code) {
    if(!value) throw OrchardRequestError(code);
}
void Exec(sqlite3* db,const char* sql) { Check(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK); }
struct Transaction {
    sqlite3* db;bool complete=false;
    explicit Transaction(sqlite3* p):db(p){Check(db&&sqlite3_get_autocommit(db));Exec(db,"BEGIN IMMEDIATE");}
    ~Transaction(){if(!complete&&sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK&&!sqlite3_get_autocommit(db))std::terminate();}
    void Commit(){Exec(db,"COMMIT");complete=true;}
};
orchard::WalletStorageIdentity Identity(const std::string& id,const OrchardAccountDelivery::Profile& p){
    Check(id.size()==71&&id.substr(0,7)=="DNWI01:"&&p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
    orchard::Hash hash{};
    const auto digit=[](char c)->uint8_t {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("Orchard account wallet identity malformed");};
    for(size_t i=0;i<hash.size();++i)hash[i]=(digit(id[7+2*i])<<4)|digit(id[8+2*i]);
    return {static_cast<orchard::WalletNetwork>(p.domain.network_code),p.domain.genesis_wire,hash,p.account};
}
// Issuance cannot use EnsureDeliveryIdentity: absent historical ownership
// must refuse without enrollment, including under a caller-owned transaction.
orchard::WalletStorageIdentity ExistingIdentity(sqlite3* db,const OrchardAccountDelivery::Profile& p,bool require_retained=true){
    Check(db&&!sqlite3_get_autocommit(db));
    struct Statement { sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);} } q;
    Check(sqlite3_prepare_v2(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1",-1,&q.p,nullptr)==SQLITE_OK);
    Check(sqlite3_step(q.p)==SQLITE_ROW&&sqlite3_column_type(q.p,0)==SQLITE_BLOB&&sqlite3_column_bytes(q.p,0)==32);
    const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q.p,0));Check(bytes);
    orchard::Hash id{};std::copy_n(bytes,32,id.begin());
    Check(std::any_of(id.begin(),id.end(),[](auto byte){return byte!=0;})&&sqlite3_step(q.p)==SQLITE_DONE);
    if(require_retained){
    Statement retained;
    Check(sqlite3_prepare_v2(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='orchard_wallet_retained'",-1,&retained.p,nullptr)==SQLITE_OK);
    Check(sqlite3_step(retained.p)==SQLITE_ROW&&sqlite3_step(retained.p)==SQLITE_DONE);
    }
    return {static_cast<orchard::WalletNetwork>(p.domain.network_code),p.domain.genesis_wire,id,p.account};
}
// Digest is an exact request equality binding inside authenticated storage,
// not an authorization supplied by callers. No secret or randomized plan input
// is regenerated from it. Explicit widths/counts distinguish all field splits.
orchard::Hash SpendRequestCommitment(const orchard::WalletStorageIdentity& identity,
        const OrchardAccountDelivery::Profile& p,const orchard::Hash& id,
        std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
    Check(payments.size()<=DINERO_ORCHARD_V1_MAX_ACTIONS&&outputs.size()<=1024);
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    Check(context&&EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)==1);
    const auto raw=[&](std::span<const uint8_t> bytes){Check(EVP_DigestUpdate(context.get(),bytes.data(),bytes.size())==1);};
    const auto number=[&](uint64_t value,size_t width){std::array<uint8_t,8> bytes{};for(size_t i=0;i<width;++i)bytes[i]=value>>(8*i);raw(std::span<const uint8_t>(bytes).first(width));};
    constexpr std::array<uint8_t,8> domain{'D','N','O','R','S','Q','0','1'};raw(domain);
    number(static_cast<uint8_t>(identity.network),1);raw(identity.genesis);raw(identity.wallet_id);
    number(identity.account,4);number(p.domain.branch_id,4);number(p.activation,4);raw(id);
    number(0,4); // Fixed locktime for this pure Orchard-input host API.
    number(payments.size(),4);
    for(const auto& payment:payments){number(payment.amount_una,8);raw(payment.recipient.Raw());raw(payment.memo);}
    number(outputs.size(),4);
    for(const auto& output:outputs){
        Check(!output.script_pub_key.empty()&&output.script_pub_key.size()<=10000);
        number(output.amount_una,8);number(output.script_pub_key.size(),4);raw(output.script_pub_key);
    }
    number(fee,8);orchard::Hash result{};unsigned size=0;
    Check(EVP_DigestFinal_ex(context.get(),result.data(),&size)==1&&size==result.size()&&result!=orchard::Hash{});
    return result;
}
// Shielding adds exact ordered transparent inputs to the authenticated request
// commitment. The distinct domain cannot collide with the pure Orchard spend
// request protocol. This binds intent, not unspentness or permission to retry.
orchard::Hash ShieldRequestCommitment(const orchard::WalletStorageIdentity& identity,
        const OrchardAccountDelivery::Profile& p,const orchard::Hash& id,
        std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
    Check(!inputs.empty()&&inputs.size()<=1024&&!payments.empty());
    const auto recipients=SpendRequestCommitment(identity,p,id,payments,outputs,fee);
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    Check(context&&EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)==1);
    const auto raw=[&](std::span<const uint8_t> bytes){Check(EVP_DigestUpdate(context.get(),bytes.data(),bytes.size())==1);};
    const auto number=[&](uint64_t value,size_t width){std::array<uint8_t,8> bytes{};
        for(size_t i=0;i<width;++i)bytes[i]=value>>(8*i);raw(std::span<const uint8_t>(bytes).first(width));};
    constexpr std::array<uint8_t,8> domain{'D','N','O','R','S','H','0','1'};raw(domain);raw(recipients);number(inputs.size(),4);
    for(const auto& input:inputs){
        Check(input.sequence==UINT32_MAX&&input.amount_una>0&&input.amount_una<=orchard::kMaxMoneyUna&&
              !input.script_pub_key.empty()&&input.script_pub_key.size()<=10000);
        raw(input.txid_wire);number(input.output_index,4);number(input.sequence,4);
        number(input.amount_una,8);number(input.script_pub_key.size(),4);raw(input.script_pub_key);
    }
    orchard::Hash result{};unsigned size=0;
    Check(EVP_DigestFinal_ex(context.get(),result.data(),&size)==1&&size==result.size()&&result!=orchard::Hash{});
    return result;
}
std::vector<orchard::WalletPayment> StoredShieldPayments(
        const OrchardOperationQueue::ShieldRequest& request,orchard::WalletNetwork network){
    std::vector<orchard::WalletPayment> payments;payments.reserve(request.payments.size());
    for(const auto& payment:request.payments)
        payments.push_back({payment.amount_una,orchard::WalletReceiver::DecodeAddress(payment.address,network),payment.memo});
    return payments;
}
OrchardOperationQueue::ShieldRequest StoreShieldRequest(
        orchard::WalletNetwork network,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
    OrchardOperationQueue::ShieldRequest request{{},{outputs.begin(),outputs.end()},fee};
    request.payments.reserve(payments.size());
    for(const auto& payment:payments)
        request.payments.push_back({payment.amount_una,payment.recipient.EncodeAddress(network),payment.memo});
    return request;
}
void ValidateBoundRequests(const OrchardOperationQueue& operations,
        const orchard::WalletStorageIdentity& identity,const OrchardAccountDelivery::Profile& profile){
    for(const auto& [id,entry]:operations.Entries()) {
        if(entry.shield_request) {
            const auto& request=*entry.shield_request;
            const auto payments=StoredShieldPayments(request,identity.network);
            Check(entry.request_commitment==ShieldRequestCommitment(identity,profile,id,
                entry.inputs,payments,request.outputs,request.fee_una));
        }
        if(entry.spend_request) {
            const auto& request=*entry.spend_request;
            const auto payments=StoredShieldPayments(request,identity.network);
            Check(entry.request_commitment==SpendRequestCommitment(identity,profile,id,
                payments,request.outputs,request.fee_una));
        }
    }
}
// Retention includes revisions outside the linked undo chain. Their sealed
// request details still belong to the original account, including records
// whose storage wallet ID is derived for an operation archive. Validate every
// captured queue before any caller can return a catalog or authorize effects.
void ValidateCatalogRequests(const OrchardOwnershipInventory::Account& account){
    const auto& entry=account.entry;
    const OrchardAccountDelivery::Profile profile{
        {entry.network,entry.genesis,entry.branch},entry.activation,entry.account};
    const auto validate=[&](const OrchardOperationQueue& queue){
        ValidateBoundRequests(queue,account.identity,profile);
    };
    validate(account.current.metadata.Operations());
    for(const auto& archived:account.current.archive)validate(archived.record.operation);
    for(const auto& retained:account.retained_accounts)validate(retained.metadata.Operations());
    for(const auto& retained:account.retained_archives)validate(retained.record.operation);
}
// This helper is reachable only after the request and exact owned proof have
// been authenticated by SignShieldProofForReplay. It uses the Orchard profile
// digest, never the ordinary transaction signer or a caller-supplied hash.
std::vector<std::vector<uint8_t>> SignShieldInput(const SigningKey& key,const orchard::Hash& digest){
    Check(key.secret.size()==32);
    std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> context(
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN|SECP256K1_CONTEXT_VERIFY),secp256k1_context_destroy);
    Check(bool(context));
    if(key.script.size()==34&&key.script[0]==0x51&&key.script[1]==0x20){
        Check(key.policy==SigningKeyPolicy::TaprootCanonical||key.policy==SigningKeyPolicy::TaprootHistoricalImport);
        struct Pair{secp256k1_keypair value{};~Pair(){OPENSSL_cleanse(&value,sizeof(value));}} pair;
        Check(secp256k1_keypair_create(context.get(),&pair.value,key.secret.data())==1);
        secp256k1_xonly_pubkey internal{},output{};
        std::array<uint8_t,33> bytes{};std::array<uint8_t,32> tweak{},public_bytes{};
        Check(secp256k1_keypair_xonly_pub(context.get(),&internal,nullptr,&pair.value)==1&&
              secp256k1_xonly_pubkey_serialize(context.get(),bytes.data(),&internal)==1);
        if(key.policy==SigningKeyPolicy::TaprootCanonical)crypto::TaggedHash("TapTweak",bytes.data(),32,tweak.data());
        else Check(::SHA256(bytes.data(),bytes.size(),tweak.data())!=nullptr);
        // The historical policy keeps its exact old tweak. Keypair APIs own
        // even-Y normalization; no tweaked scalar is exported or cached.
        Check(secp256k1_keypair_xonly_tweak_add(context.get(),&pair.value,tweak.data())==1&&
              secp256k1_keypair_xonly_pub(context.get(),&output,nullptr,&pair.value)==1&&
              secp256k1_xonly_pubkey_serialize(context.get(),public_bytes.data(),&output)==1&&
              std::equal(public_bytes.begin(),public_bytes.end(),key.script.begin()+2));
        struct Aux{std::array<uint8_t,32> value{};~Aux(){OPENSSL_cleanse(value.data(),value.size());}} aux;
        std::vector<uint8_t> signature(64);
        Check(RAND_bytes(aux.value.data(),int(aux.value.size()))==1&&
              secp256k1_schnorrsig_sign32(context.get(),signature.data(),digest.data(),&pair.value,aux.value.data())==1&&
              secp256k1_schnorrsig_verify(context.get(),signature.data(),digest.data(),digest.size(),&output)==1);
        return {std::move(signature)};
    }
    Check(key.policy==SigningKeyPolicy::Untweaked&&key.script.size()==22&&key.script[0]==0&&key.script[1]==20);
    secp256k1_pubkey public_key{};std::vector<uint8_t> public_bytes(33);size_t size=public_bytes.size();
    Check(secp256k1_ec_pubkey_create(context.get(),&public_key,key.secret.data())==1&&
          secp256k1_ec_pubkey_serialize(context.get(),public_bytes.data(),&size,&public_key,SECP256K1_EC_COMPRESSED)==1&&size==33);
    const auto hash=din::crypto::HASH160(public_bytes);Check(std::equal(hash.begin(),hash.end(),key.script.begin()+2));
    secp256k1_ecdsa_signature signature{};std::vector<uint8_t> der(72);size=der.size();
    Check(secp256k1_ecdsa_sign(context.get(),&signature,digest.data(),key.secret.data(),nullptr,nullptr)==1&&
          secp256k1_ecdsa_signature_normalize(context.get(),nullptr,&signature)==0&&
          secp256k1_ecdsa_verify(context.get(),&signature,digest.data(),&public_key)==1&&
          secp256k1_ecdsa_signature_serialize_der(context.get(),der.data(),&size,&signature)==1);
    der.resize(size);der.push_back(1);return {std::move(der),std::move(public_bytes)};
}
void CheckShieldSnapshot(const consensus::OrchardCoinSnapshot& snapshot,
        std::span<const orchard::ResolvedInput> requested){
    const auto& envelope=snapshot.Transaction();
    Check(!requested.empty()&&envelope.LockTime()==0&&envelope.Inputs().size()==requested.size()&&
          snapshot.Coins().size()==requested.size());
    for(size_t i=0;i<requested.size();++i){const auto& input=envelope.Inputs()[i];const auto& coin=snapshot.Coins()[i];const auto& request=requested[i];
        Check(input.txid_wire==request.txid_wire&&input.output_index==request.output_index&&
              input.sequence==UINT32_MAX&&request.sequence==UINT32_MAX&&input.script_sig.empty()&&
              coin.value.GetUna()==request.amount_una&&coin.scriptPubKey==request.script_pub_key&&
              !coin.is_confidential&&coin.commitment.empty());
    }
}
void RequireShieldFullSync(sqlite3* db){
    Check(db&&sqlite3_get_autocommit(db));Exec(db,"PRAGMA synchronous=FULL");
    struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
    Check(sqlite3_prepare_v2(db,"PRAGMA synchronous",-1,&q.p,nullptr)==SQLITE_OK);
    Check(sqlite3_step(q.p)==SQLITE_ROW&&sqlite3_column_type(q.p,0)==SQLITE_INTEGER&&
          sqlite3_column_int64(q.p,0)==2&&sqlite3_step(q.p)==SQLITE_DONE);
}
} // namespace
struct OrchardAccountDelivery::Owner {
    static WalletManager::DatabaseLease::ShieldHistory ShieldHistory(const OrchardOperationQueue::Entry& entry) {
        Check(entry.phase==OrchardOperationQueue::Phase::Ready&&entry.shield_request&&entry.shield_ready_time&&
              *entry.shield_ready_time>0&&*entry.shield_ready_time<=uint64_t(INT64_MAX));
        const auto& request=*entry.shield_request;Check(!request.payments.empty());uint64_t debit=request.fee_una;
        for(const auto& payment:request.payments){Check(payment.amount_una<=orchard::kMaxMoneyUna-debit);debit+=payment.amount_una;}
        const auto wire=orchard::TransactionEnvelope::DecodeExact(entry.transaction).Txid();uint256 id;
        std::copy(wire.begin(),wire.end(),id.begin());
        return {id.GetHex(),request.payments.size()==1?request.payments.front().address:std::string(),debit,static_cast<int64_t>(*entry.shield_ready_time)};
    }
    void ValidateShieldHistories(const OrchardOperationQueue& queue) {
        for(const auto& [id,entry]:queue.Entries())if(entry.shield_ready_time)
            lease->ValidateShieldHistoryInTransaction(*seed,ShieldHistory(entry));
    }
    struct ViewingKey {
        orchard::FullViewingKeyBytes bytes;
        explicit ViewingKey(const orchard::WalletKeys& keys):bytes(keys.ExportFullViewingKey()){}
        ~ViewingKey(){OPENSSL_cleanse(bytes.data(),bytes.size());}
    };
    std::unique_ptr<WalletManager::DatabaseLease> lease;
    std::unique_ptr<WalletManager::RecoverySeed> seed;
    std::optional<OrchardAccountCatalog::Snapshot> catalog;
    orchard::WalletStorageIdentity identity;
    orchard::WalletKeys keys;
    ViewingKey fvk;
    orchard::WalletSnapshotStore store;
    Owner(WalletManager& wallet,uint64_t session,const OrchardAccountDelivery::Profile& p,bool existing_only=false,bool creating=false)
        :lease(wallet.AcquireDatabaseLease()),seed(),identity(Bind(session,p,existing_only,creating)),
         keys(orchard::WalletKeys::FromSeed(seed->Bytes(),p.account)),fvk(keys),
         store(lease->Database(),identity,seed->Bytes()){}
    orchard::WalletStorageIdentity Bind(uint64_t session,const OrchardAccountDelivery::Profile& p,bool existing_only,bool creating){
        Check(lease->Session()==session&&lease->Database());
        Check(p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
        // Refuse missing/locked keys before identity initialization can write.
        seed=lease->CopyRecoverySeed(session);
        if(creating){
            Check(!sqlite3_get_autocommit(lease->Database()));
            catalog=OrchardAccountCatalog::Read(lease->Database(),seed->Bytes());
            Check(catalog&&catalog->generated&&catalog->accounts.size()<OrchardAccountCatalog::kMaxAccounts);
            Check(std::none_of(catalog->accounts.begin(),catalog->accounts.end(),[&](const auto& e){return e.account==p.account;}));
            const auto identity=ExistingIdentity(lease->Database(),p,!catalog->accounts.empty());
            // Only the authenticated genuinely empty catalog may initialize
            // absent schema. A recorded owner never permits missing schema repair.
            if(catalog->accounts.empty()){
                struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} table,rows;
                Check(sqlite3_prepare_v2(lease->Database(),"SELECT 1 FROM sqlite_master WHERE type='table' AND name='orchard_wallet_retained'",-1,&table.p,nullptr)==SQLITE_OK);
                const int rc=sqlite3_step(table.p);
                if(rc==SQLITE_ROW){
                    Check(sqlite3_step(table.p)==SQLITE_DONE);
                    Check(sqlite3_prepare_v2(lease->Database(),"SELECT 1 FROM orchard_wallet_retained",-1,&rows.p,nullptr)==SQLITE_OK);
                    Check(sqlite3_step(rows.p)==SQLITE_DONE);
                }else Check(rc==SQLITE_DONE);
                orchard::WalletSnapshotStore::InitializeSchemaUnderTransaction(lease->Database());
            }
            return identity;
        }

        return existing_only?ExistingIdentity(lease->Database(),p):Identity(lease->EnsureDeliveryIdentity(),p);
    }
    // Own only authenticated plaintext and the derived viewing key. Neither
    // the seed pin, SQLite connection nor wallet lease escapes the capture.
    // A caller's outer lease is still its responsibility; existing composite
    // writers must be migrated before replay may lazily verify proofs there.
    struct ExistingRead {};
    // The outer caller lease and transaction already exist. A read may not
    // enroll a missing persistent identity or initialize absent schema.
    Owner(WalletManager& wallet,uint64_t session,const Profile& p,ExistingRead)
        :lease(wallet.AcquireDatabaseLease()),seed(),identity(BindExistingRead(session,p)),
         keys(orchard::WalletKeys::FromSeed(seed->Bytes(),p.account)),fvk(keys),
         store(lease->Database(),identity,seed->Bytes()){}
    orchard::WalletStorageIdentity BindExistingRead(uint64_t session,const Profile& p){
        Check(lease->Session()==session&&lease->Database()&&!sqlite3_get_autocommit(lease->Database()));
        Check(p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
        seed=lease->CopyRecoverySeed(session);
        // Read() historically accepts snapshots predating retained history.
        // Do not add a retained-table requirement to this scoped account read.
        return ExistingIdentity(lease->Database(),p,false);
    }
    struct CapturedRead {
        orchard::LoadedWalletState saved;
        orchard::WalletStorageIdentity identity;
        std::shared_ptr<const void> instance;
        orchard::FullViewingKeyBytes viewing;
        CapturedRead(orchard::LoadedWalletState value,
                orchard::WalletStorageIdentity id,std::shared_ptr<const void> token,
                const orchard::FullViewingKeyBytes& key)
            :saved(std::move(value)),identity(id),instance(std::move(token)),viewing(key){}
        ~CapturedRead(){OPENSSL_cleanse(viewing.data(),viewing.size());}
        CapturedRead(const CapturedRead&)=delete;
        CapturedRead& operator=(const CapturedRead&)=delete;
        OrchardAccountDelivery::Applied Restore(const Profile& p,const RestorePoint& point) const {
            auto account=OrchardAccountState::Restore(saved.state,p.domain,viewing,p.activation,
                point.checkpoint,point.lookups);
            Check(account.ParentSnapshotRevision()<saved.revision);
            ValidateBoundRequests(account.Operations(),identity,p);
            return {saved.revision,std::move(account)};
        }
    };
    std::unique_ptr<CapturedRead> CaptureRead(const Profile& p){
        auto saved=store.Read();Check(saved.has_value());
        const auto metadata=OrchardAccountMetadata::Read(saved->state,p.domain,fvk.bytes,p.activation);
        Check(metadata.ParentSnapshotRevision()<saved->revision);
        ValidateBoundRequests(metadata.Operations(),identity,p);
        ValidateShieldHistories(metadata.Operations());
        return std::make_unique<CapturedRead>(std::move(*saved),identity,lease->InstanceToken(),fvk.bytes);
    }
    void RecheckRead(const CapturedRead& captured,const Applied& restored){
        Check(lease->InstanceToken()==captured.instance&&identity.network==captured.identity.network&&
              identity.genesis==captured.identity.genesis&&identity.wallet_id==captured.identity.wallet_id&&
              identity.account==captured.identity.account&&
              CRYPTO_memcmp(fvk.bytes.data(),captured.viewing.data(),fvk.bytes.size())==0);
        const auto current=store.Read();Check(current&&current->revision==captured.saved.revision&&
            restored.revision==captured.saved.revision);
        const auto expected=captured.saved.state.Bytes(),actual=current->state.Bytes();
        Check(expected.size()==actual.size()&&
              CRYPTO_memcmp(expected.data(),actual.data(),expected.size())==0);
        // Ordinary Ready history is a separate owner. Authenticate it again
        // before returning a result prepared outside this wallet transaction.
        ValidateShieldHistories(restored.account.Operations());
    }
    OrchardAccountDelivery::Applied Restore(const OrchardAccountDelivery::Profile& p,
            const OrchardAccountDelivery::RestorePoint& point){
        auto saved=store.Read();Check(saved.has_value());
        auto account=OrchardAccountState::Restore(saved->state,p.domain,fvk.bytes,p.activation,point.checkpoint,point.lookups);
        Check(account.ParentSnapshotRevision()<saved->revision);
        ValidateBoundRequests(account.Operations(),identity,p);
        ValidateShieldHistories(account.Operations());
        return {saved->revision,std::move(account)};
    }
    OrchardAccountDelivery::Applied RestoreReplay(const OrchardAccountDelivery::Profile& p,const RuntimeAccountReplay& view){
        const auto origin_event=view.Event(1);const auto& context=origin_event->context;
        Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
            context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
        const auto saved=store.Read();Check(saved.has_value());
        const auto receipt=OrchardAccountState::ReadDeliveryMetadata(saved->state,p.domain,fvk.bytes,p.activation,view.Point({}).checkpoint.block_hash);
        // A zero receipt is usable only if the FULL encrypted scan restores
        // at the checked activation origin. It is not an applied cursor.
        auto result=Restore(p,view.Point({receipt.sequence,receipt.digest}));Check(result.revision==saved->revision);return result;
    }
    OrchardAccountDelivery::Applied Reconcile(OrchardAccountDelivery::Applied current,
            const OrchardAccountDelivery::Profile& p,const RuntimeAccountReplay& view){
        const auto receipt=current.account.Delivery();
        const auto selected=view.SelectedHashes({receipt.sequence,receipt.digest});
        OrchardOperationArchive archive(lease->Database(),identity,p.domain,seed->Bytes());
        auto cursor=archive.Begin(current.account);
        while(cursor.Remaining()){
            const auto page=archive.List(cursor,64);Check(!page.entries.empty());
            for(const auto& located:page.entries){
                const auto record=archive.Read(located.Id());
                // Fully restored pending entries already own their reservations
                // and may carry a newer observation. Never replace their bytes.
                if(current.account.Operations().Entries().contains(located.Id()))continue;
                if(record.observation.height<=current.account.Scan().Checkpoint().height){
                    const auto hash=selected(record.observation.height);
                    if(!hash.ok())throw consensus::OrchardStateLookupError(hash.status());
                    Check(!hash->IsNull());
                    if(*hash==record.observation.block_hash)continue;
                }
                // A legacy owner may already have scanned the replacement
                // branch without this operation present. Reacquire reservations
                // AND replay real bodies from the actual fork before commit.
                const RuntimeOutboxCursor position{receipt.sequence,receipt.digest};
                const auto fork=view.ForkHeight(position,record.observation.block_hash,record.observation.height);
                auto staged=archive.StageReactivate(current.revision,current.account,located,selected);
                current={staged.revision,std::move(staged.account)};
                auto observed=current.account.ObserveReactivatedOperation(located.Id(),fork,view.Point(position).lookups,selected);
                if(observed.Observations()!=current.account.Observations())
                    current=Replace(current.revision,std::move(observed));
            }
            cursor=page.next;
        }
        return current;
    }
    OrchardAccountDelivery::Applied Undo(const OrchardAccountDelivery::Profile& p,
            const OrchardAccountDelivery::Applied& current,const RuntimeOutboxEvent& event,
            const OrchardBlockCandidate& block,const OrchardAccountDelivery::RestorePoint& parent){
        const auto parent_revision=current.account.ParentSnapshotRevision();
        Check(parent_revision&&parent_revision<current.revision);
        auto retained=store.ReadRetained(parent_revision);
        auto prior=OrchardAccountState::Restore(retained.state,p.domain,fvk.bytes,p.activation,parent.checkpoint,parent.lookups);
        Check(prior.ParentSnapshotRevision()<parent_revision);
        return Replace(current.revision,current.account.RewindDelivery(event,block,prior)
            .WithParentSnapshotRevision(prior.ParentSnapshotRevision()));
    }
    // Own authenticated bytes for the entire declared catalog. No seed,
    // SQLite connection or wallet lease survives capture. Restoring scans is
    // deliberately separate from acquiring and checking these owners.
    struct CapturedCatalog {
        struct ArchiveBytes {
            orchard::WalletStorageIdentity identity;
            bool retained;
            orchard::LoadedWalletState saved;
        };
        struct AccountBytes {
            ViewingKey viewing;
            orchard::LoadedWalletState current;
            std::vector<orchard::LoadedWalletState> retained;
            std::vector<ArchiveBytes> archives;
            AccountBytes(const orchard::WalletKeys& keys,orchard::LoadedWalletState saved)
                :viewing(keys),current(std::move(saved)){}
        };
        OrchardOwnershipInventory::Snapshot ownership;
        std::shared_ptr<const void> instance;
        std::vector<std::unique_ptr<AccountBytes>> bytes;
        CapturedCatalog(OrchardOwnershipInventory::Snapshot snapshot,std::shared_ptr<const void> token)
            :ownership(std::move(snapshot)),instance(std::move(token)){}
        CapturedCatalog(const CapturedCatalog&)=delete;
        CapturedCatalog& operator=(const CapturedCatalog&)=delete;
        static bool Equal(const orchard::WalletStateBytes& a,const orchard::WalletStateBytes& b){
            return a.Bytes().size()==b.Bytes().size()&&
                (a.Bytes().empty()||CRYPTO_memcmp(a.Bytes().data(),b.Bytes().data(),a.Bytes().size())==0);
        }
        static bool SameIdentity(const orchard::WalletStorageIdentity& a,const orchard::WalletStorageIdentity& b){
            return a.network==b.network&&a.genesis==b.genesis&&a.wallet_id==b.wallet_id&&a.account==b.account;
        }
        static void Unchanged(bool value){
            if(!value)throw OrchardAccountDelivery::CatalogChanged();
        }
        static void SameState(const orchard::LoadedWalletState& a,const orchard::LoadedWalletState& b){
            Unchanged(a.revision==b.revision&&Equal(a.state,b.state));
        }
        void Recheck(const CapturedCatalog& fresh) const {
            Check(instance==fresh.instance&&ownership.wallet_id==fresh.ownership.wallet_id);
            Unchanged(ownership.catalog==fresh.ownership.catalog&&ownership.current_rows==fresh.ownership.current_rows&&
                ownership.retained_rows==fresh.ownership.retained_rows&&bytes.size()==fresh.bytes.size());
            for(size_t i=0;i<bytes.size();++i){
                const auto& a=*bytes[i];const auto& b=*fresh.bytes[i];
                Check(SameIdentity(ownership.accounts[i].identity,fresh.ownership.accounts[i].identity)&&
                    CRYPTO_memcmp(a.viewing.bytes.data(),b.viewing.bytes.data(),a.viewing.bytes.size())==0);
                SameState(a.current,b.current);
                Unchanged(a.retained.size()==b.retained.size()&&a.archives.size()==b.archives.size());
                for(size_t j=0;j<a.retained.size();++j)SameState(a.retained[j],b.retained[j]);
                for(size_t j=0;j<a.archives.size();++j){
                    Check(a.archives[j].retained==b.archives[j].retained&&
                        SameIdentity(a.archives[j].identity,b.archives[j].identity));
                    SameState(a.archives[j].saved,b.archives[j].saved);
                }
            }
        }
        CatalogEnrolled Restore(const std::function<RestorePoint(RuntimeOutboxCursor)>& points) const {
            const auto origin=points({}).checkpoint.block_hash;
            CatalogEnrolled result{ownership.catalog,{}};
            result.accounts.reserve(bytes.size());
            size_t predecessors=0;
            for(size_t i=0;i<bytes.size();++i){
                const auto& captured=ownership.accounts[i];const auto& payload=*bytes[i];
                const auto& entry=captured.entry;
                const Profile profile{{entry.network,entry.genesis,entry.branch},entry.activation,entry.account};
                const auto restore=[&](const orchard::LoadedWalletState& saved){
                    const auto receipt=OrchardAccountState::ReadDeliveryMetadata(saved.state,profile.domain,
                        payload.viewing.bytes,profile.activation,origin);
                    const auto point=points({receipt.sequence,receipt.digest});
                    auto state=OrchardAccountState::Restore(saved.state,profile.domain,payload.viewing.bytes,
                        profile.activation,point.checkpoint,point.lookups);
                    Check(state.ParentSnapshotRevision()<saved.revision);
                    ValidateBoundRequests(state.Operations(),captured.identity,profile);
                    return state;
                };
                auto current=restore(payload.current);
                // Preserve the exact current-owner check formerly performed
                // by OrchardOperationArchive::Begin under the transaction.
                Check(Equal(payload.current.state,current.Encode())&&
                    current.Archive()==captured.current.metadata.Archive()&&
                    current.Delivery()==captured.current.metadata.Delivery()&&
                    current.ParentSnapshotRevision()==captured.current.metadata.ParentSnapshotRevision());
                std::map<uint64_t,const orchard::LoadedWalletState*> retained;
                for(const auto& saved:payload.retained)Check(retained.emplace(saved.revision,&saved).second);
                auto revision=current.ParentSnapshotRevision();auto upper=payload.current.revision;
                while(revision){
                    Check(++predecessors<=65536&&revision<upper);
                    const auto found=retained.find(revision);Check(found!=retained.end());
                    auto parent=restore(*found->second);upper=revision;revision=parent.ParentSnapshotRevision();
                }
                std::vector<std::pair<orchard::Hash,uint64_t>> archives;
                for(const auto& record:captured.current.archive)
                    archives.emplace_back(record.identity.wallet_id,record.record.revision);
                result.accounts.push_back({entry.account,{payload.current.revision,std::move(current)},std::move(archives)});
            }
            return result;
        }
    };
    static std::unique_ptr<CapturedCatalog> CaptureCatalog(WalletManager& wallet,uint64_t session,const Profile& profile){
        auto lease=wallet.AcquireDatabaseLease();
        Check(lease->Session()==session&&lease->Database()&&!sqlite3_get_autocommit(lease->Database()));
        auto seed=lease->CopyRecoverySeed(session);
        auto result=std::make_unique<CapturedCatalog>(
            OrchardOwnershipInventory::Read(lease->Database(),seed->Bytes()),lease->InstanceToken());
        const auto histories=[&](const OrchardOperationQueue& queue){
            for(const auto& [id,entry]:queue.Entries())if(entry.shield_ready_time)
                lease->ValidateShieldHistoryInTransaction(*seed,ShieldHistory(entry));
        };
        for(const auto& account:result->ownership.accounts){
            const auto& entry=account.entry;
            Check(entry.network==profile.domain.network_code&&entry.genesis==profile.domain.genesis_wire&&
                entry.branch==profile.domain.branch_id&&entry.activation==profile.activation);
            const Profile p{profile.domain,profile.activation,entry.account};
            ValidateCatalogRequests(account);
            const auto validate=[&](const OrchardOperationQueue& queue){
                ValidateBoundRequests(queue,account.identity,p);histories(queue);
            };
            validate(account.current.metadata.Operations());
            const auto keys=orchard::WalletKeys::FromSeed(seed->Bytes(),entry.account);
            orchard::WalletSnapshotStore store(lease->Database(),account.identity,seed->Bytes());
            auto saved=store.Read();Check(saved&&saved->revision==account.current.revision);
            auto bytes=std::make_unique<CapturedCatalog::AccountBytes>(keys,std::move(*saved));
            std::map<uint64_t,const OrchardAccountMetadata*> parents;
            for(const auto& retained:account.retained_accounts){
                bytes->retained.push_back(store.ReadRetained(retained.revision));
                Check(parents.emplace(retained.revision,&retained.metadata).second);
            }
            // Preserve the existing ordinary-history obligations for the
            // selected current account, reached archives and linked parents.
            auto parent=account.current.metadata.ParentSnapshotRevision();
            while(parent){
                const auto found=parents.find(parent);Check(found!=parents.end());
                validate(found->second->Operations());parent=found->second->ParentSnapshotRevision();
            }
            std::map<orchard::Hash,std::vector<uint64_t>> archived_revisions;
            for(const auto& retained:account.retained_archives)
                archived_revisions[retained.id].push_back(retained.record.revision);
            for(const auto& archived:account.current.archive){
                validate(archived.record.operation);
                orchard::WalletSnapshotStore archive(lease->Database(),archived.identity,seed->Bytes());
                auto record=archive.Read();Check(record&&record->revision==archived.record.revision);
                bytes->archives.push_back({archived.identity,false,std::move(*record)});
                const auto history=archived_revisions.find(archived.id);
                if(history!=archived_revisions.end())for(const auto revision:history->second)
                    bytes->archives.push_back({archived.identity,true,archive.ReadRetained(revision)});
            }
            result->bytes.push_back(std::move(bytes));
        }
        return result;
    }
    std::vector<OrchardAccountDelivery::Enrolled> ReadInventory(const RuntimeAccountReplay&,bool allow_empty);
    void ValidateRetainedInventory(const std::vector<OrchardAccountDelivery::Enrolled>&,const RuntimeAccountReplay&);
    std::vector<OrchardAccountDelivery::Enrolled> ValidateCatalogInventory(const OrchardAccountDelivery::Profile&,const RuntimeAccountReplay&);
    // Each temporary owner releases its seed pin before the next account is
    // opened. The caller's outer FULL transaction/lease spans the entire pass.
    static void ReconcileSpendCatalog(WalletManager&,uint64_t,const OrchardAccountDelivery::Profile&,
        const RuntimeAccountReplay&,const std::optional<orchard::Hash>& new_operation);
    OrchardAccountDelivery::Applied Replace(uint64_t expected,OrchardAccountState account){
        auto encoded=account.Encode();const auto revision=store.StageReplaceRetaining(expected,encoded);
        return {revision,std::move(account)};
    }
};
struct OrchardCatalogFinalizationPlan::Data {
    std::unique_ptr<OrchardAccountDelivery::Owner::CapturedCatalog> captured;
    OrchardAccountDelivery::Profile profile;
    uint64_t session;
    orchard::Hash operation;
    size_t index;
    OrchardAccountDelivery::Applied current;
    OrchardProofJobs* jobs;
    bool shield;
    std::vector<orchard::ResolvedInput> inputs;
    std::vector<orchard::TransparentOutput> outputs;
    uint64_t fee;
    std::optional<OrchardProofJobs::State> job_state;
    std::unique_ptr<orchard::ProvedWalletBundle> proof;
    std::shared_ptr<const void> job_token;
};
OrchardCatalogFinalizationPlan::OrchardCatalogFinalizationPlan(std::unique_ptr<Data> data):data_(std::move(data)){}
OrchardCatalogFinalizationPlan::~OrchardCatalogFinalizationPlan()=default;
struct OrchardCatalogCapture::Data {
    std::unique_ptr<OrchardAccountDelivery::Owner::CapturedCatalog> captured;
    std::shared_ptr<const RuntimeAccountReplay> replay;
    OrchardAccountDelivery::Profile profile;
    uint64_t session;
};
OrchardCatalogCapture::OrchardCatalogCapture(std::unique_ptr<Data> data):data_(std::move(data)){}
OrchardCatalogCapture::~OrchardCatalogCapture()=default;
std::unique_ptr<OrchardCatalogCapture> OrchardAccountDelivery::PrepareCatalogForReplay(
        WalletManager& w,uint64_t session,std::shared_ptr<const RuntimeAccountReplay> replay){
    Check(bool(replay));
    const auto points=[replay](RuntimeOutboxCursor cursor){return replay->Point(cursor);};
    return PrepareCatalogWithRestorePoints(w,session,std::move(replay),points);
}
std::unique_ptr<OrchardCatalogCapture> OrchardAccountDelivery::PrepareCatalogWithRestorePoints(
        WalletManager& w,uint64_t session,std::shared_ptr<const RuntimeAccountReplay> replay,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    Check(bool(replay)&&bool(points));
    const auto first=replay->Event(1);const auto& context=first->context;
    const Profile profile{context.domain,context.activation_height,0};
    std::unique_ptr<Owner::CapturedCatalog> captured;
    {
        auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        captured=Owner::CaptureCatalog(w,session,profile);tx.Commit();
    }
    // Full current and linked-parent restoration is deliberately before the
    // service acquires selected ownership or the writer opens its transaction.
    // Restored mutable state is not handed to the writer as an authority token.
    (void)captured->Restore(points);
    auto data=std::make_unique<OrchardCatalogCapture::Data>();
    data->captured=std::move(captured);data->replay=std::move(replay);
    data->profile=profile;data->session=session;
    return std::unique_ptr<OrchardCatalogCapture>(new OrchardCatalogCapture(std::move(data)));
}
void OrchardAccountDelivery::RecheckCatalogCaptureInTransaction(WalletManager& w,uint64_t session,
        const std::shared_ptr<const RuntimeAccountReplay>& replay,const OrchardCatalogCapture& prepared){
    Check(prepared.data_&&prepared.data_->captured&&replay&&
        prepared.data_->replay==replay&&prepared.data_->session==session);
    auto lease=w.AcquireDatabaseLease();
    Check(lease->Session()==session&&lease->Database()&&!sqlite3_get_autocommit(lease->Database()));
    // Authenticate every current/reached/retained row, catalog, persistent
    // identity, instance, key and ordinary shield history in this exact SQL
    // snapshot. No replay Point/Event/body/proof callback is made under it.
    auto fresh=Owner::CaptureCatalog(w,session,prepared.data_->profile);
    prepared.data_->captured->Recheck(*fresh);
}
struct OrchardCatalogRecoveryPlan::Data {
    struct Step { bool retaining; orchard::LoadedWalletState next; };
    struct Account { OrchardAccountDelivery::Applied result; std::vector<Step> steps; };
    std::unique_ptr<OrchardAccountDelivery::Owner::CapturedCatalog> expected;
    std::shared_ptr<const RuntimeAccountReplay> replay;
    OrchardAccountDelivery::Profile profile;
    uint64_t session;
    OrchardAccountDelivery::CatalogEnrolled before;
    std::vector<Account> accounts;
    size_t next_account=0;
    bool usable=true;
    bool ordinary_payment=false;
    RuntimeOutboxCursor head;
    struct Shield {
        OrchardAccountDelivery::Profile profile;
        orchard::Hash id{};
        uint64_t fee=0,deposit=0;
        size_t index=0;
        std::vector<orchard::WalletPayment> payments;
        bool bound=false,existing_only=false;
        std::optional<orchard::Hash> exact_request;
        std::unique_ptr<OrchardAccountDelivery::QueuedSpend> existing;
    };
    std::unique_ptr<Shield> shield;
};
OrchardCatalogRecoveryPlan::OrchardCatalogRecoveryPlan(std::unique_ptr<Data> data):data_(std::move(data)){}
OrchardCatalogRecoveryPlan::~OrchardCatalogRecoveryPlan()=default;
std::unique_ptr<OrchardCatalogRecoveryPlan> OrchardAccountDelivery::PrepareCatalogRecovery(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view,CatalogRecoveryAction action,uint64_t sequence){
    return PrepareCatalogRecoveryWithPoints(w,session,view,action,sequence,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
std::unique_ptr<OrchardCatalogRecoveryPlan> OrchardAccountDelivery::PrepareCatalogRecoveryWithPoints(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view,CatalogRecoveryAction action,uint64_t sequence,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    Check(bool(points)&&((action==CatalogRecoveryAction::Apply)==(sequence!=0)));
    const auto first=view.Event(1);const Profile profile{first->context.domain,first->context.activation_height,0};
    auto data=std::make_unique<OrchardCatalogRecoveryPlan::Data>();
    // RuntimeAccountReplay copies retain the same immutable shared source data.
    data->replay=std::make_shared<const RuntimeAccountReplay>(view);data->profile=profile;data->session=session;data->head=view.Head();
    {
        auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        data->expected=Owner::CaptureCatalog(w,session,profile);tx.Commit();
    }
    data->before=data->expected->Restore(points);
    auto plan=std::unique_ptr<OrchardCatalogRecoveryPlan>(new OrchardCatalogRecoveryPlan(std::move(data)));
    CompleteCatalogRecoveryWithPoints(*plan,action,sequence,points);
    return plan;
}
void OrchardAccountDelivery::CompleteCatalogRecoveryWithPoints(OrchardCatalogRecoveryPlan& plan,
        CatalogRecoveryAction action,uint64_t sequence,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    Check(plan.data_&&plan.data_->usable&&plan.data_->next_account==0&&bool(points)&&
          ((action==CatalogRecoveryAction::Apply)==(sequence!=0)));
    auto* data=plan.data_.get();
    // An observed catalog may be reconciled only before any planned mutation.
    // Matching request retries never enter this computation.
    Check(std::all_of(data->accounts.begin(),data->accounts.end(),
        [](const auto& account){return account.steps.empty();}));
    data->accounts.clear();
    const auto& view=*data->replay;const auto& profile=data->profile;
    data->accounts.reserve(data->before.accounts.size());
    for(size_t i=0;i<data->before.accounts.size();++i){
        const auto& before=data->before.accounts[i];const auto& captured=data->expected->ownership.accounts[i];
        auto& payload=*data->expected->bytes[i];
        Check(before.number==captured.entry.account);
        const Profile p{profile.domain,profile.activation,before.number};
        auto current=before.state;
        data->accounts.push_back({current,{}});auto& planned=data->accounts.back();
        const auto replace=[&](OrchardAccountState next,bool retaining){
            Check(current.revision<uint64_t(INT64_MAX));const auto revision=current.revision+1;
            auto encoded=next.Encode();planned.steps.push_back({retaining,{revision,std::move(encoded)}});
            current={revision,std::move(next)};
        };
        const auto reconcile=[&]{
            const auto receipt=current.account.Delivery();const RuntimeOutboxCursor position{receipt.sequence,receipt.digest};
            const auto selected=view.SelectedHashes(position);
            Check(current.account.Archive()==captured.current.metadata.Archive());
            // CaptureCurrent already authenticated this complete head-to-predecessor
            // sequence. Preserve the same traversal and per-reactivation revisions.
            for(const auto& archived:captured.current.archive){
                const auto& id=archived.id;const auto& record=archived.record;
                if(current.account.Operations().Entries().contains(id))continue;
                const auto& checkpoint=current.account.Scan().Checkpoint();
                if(record.observation.height<=checkpoint.height){
                    const auto hash=selected(record.observation.height);
                    if(!hash.ok())throw consensus::OrchardStateLookupError(hash.status());
                    Check(!hash->IsNull());if(*hash==record.observation.block_hash)continue;
                }
                const auto fork=view.ForkHeight(position,record.observation.block_hash,record.observation.height);
                const auto tip=selected(checkpoint.height);
                if(!tip.ok())throw consensus::OrchardStateLookupError(tip.status());
                Check(*tip==checkpoint.block_hash);
                // StageReactivate uses a NON-retaining snapshot replacement.
                // Keep that revision step, then retain only if observation replay changes state.
                replace(current.account.RestoreArchivedOperation(id,record.operation),false);
                auto observed=current.account.ObserveReactivatedOperation(id,fork,points(position).lookups,selected);
                if(observed.Observations()!=current.account.Observations())replace(std::move(observed),true);
            }
        };
        if(action==CatalogRecoveryAction::Reconcile)reconcile();
        if(action==CatalogRecoveryAction::Apply&&current.account.Delivery().sequence<sequence){
            const auto event=view.Event(sequence);reconcile();
            if(!event->IsOrchardProfile())replace(current.account.ApplyHistoricalDelivery(*event),true);
            else if(event->direction==RuntimeBlockDirection::Connect){
                const auto block=view.Block(sequence);const auto state=view.State(sequence);const auto auth=view.Authorizations(sequence);
                replace(current.account.AdvanceDelivery(*event,*block,*state,*auth).WithParentSnapshotRevision(current.revision),true);
            }else{
                const auto revision=current.account.ParentSnapshotRevision();Check(revision&&revision<current.revision);
                const auto found=std::find_if(payload.retained.begin(),payload.retained.end(),
                    [&](const auto& saved){return saved.revision==revision;});Check(found!=payload.retained.end());
                const auto parent=points(event->cursor);const auto block=view.Block(sequence);
                auto prior=OrchardAccountState::Restore(found->state,p.domain,payload.viewing.bytes,p.activation,
                    parent.checkpoint,parent.lookups);
                Check(prior.ParentSnapshotRevision()<revision);
                replace(current.account.RewindDelivery(*event,*block,prior).WithParentSnapshotRevision(prior.ParentSnapshotRevision()),true);
            }
            reconcile();
            const auto receipt=current.account.Delivery();
            Check(RuntimeOutboxCursor{receipt.sequence,receipt.digest}==event->cursor&&
                current.account.Scan().Checkpoint()==points(event->cursor).checkpoint);
        }
        planned.result=std::move(current);
        const auto retained=std::count_if(planned.steps.begin(),planned.steps.end(),[](const auto& step){return step.retaining;});
        Check(retained<=OrchardOwnershipInventory::kMaxRows&&payload.retained.size()<=OrchardOwnershipInventory::kMaxRows-retained);
        payload.retained.reserve(payload.retained.size()+retained);
    }
}
const OrchardAccountDelivery::CatalogEnrolled& OrchardAccountDelivery::CatalogRecoveryBefore(const OrchardCatalogRecoveryPlan& plan){
    Check(plan.data_&&plan.data_->usable);return plan.data_->before;
}
void OrchardAccountDelivery::RecheckCatalogRecoveryInTransaction(WalletManager& w,uint64_t session,
        const OrchardCatalogRecoveryPlan& plan){
    Check(plan.data_&&plan.data_->usable&&plan.data_->session==session);
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session&&lease->Database()&&!sqlite3_get_autocommit(lease->Database()));
    auto fresh=Owner::CaptureCatalog(w,session,plan.data_->profile);plan.data_->expected->Recheck(*fresh);
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::CommitCatalogRecoveryAccount(
        WalletManager& w,uint64_t session,OrchardCatalogRecoveryPlan& plan,size_t index){
    Check(plan.data_&&plan.data_->usable&&plan.data_->session==session&&index>=plan.data_->next_account&&index<plan.data_->accounts.size());
    auto& data=*plan.data_;data.usable=false; // Any refused write consumes this private computation.
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session&&lease->Database());
    RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
    {auto fresh=Owner::CaptureCatalog(w,session,data.profile);data.expected->Recheck(*fresh);}
    auto& planned=data.accounts[index];auto& bytes=*data.expected->bytes[index];
    const Profile p{data.profile.domain,data.profile.activation,data.before.accounts[index].number};
    Applied result=planned.result;static_assert(std::is_nothrow_move_constructible_v<Applied>);
    {
        Owner owner(w,session,p,Owner::ExistingRead{});
        for(auto& step:planned.steps){
            const auto expected=bytes.current.revision;
            const auto revision=step.retaining?owner.store.StageReplaceRetaining(expected,step.next.state):owner.store.StageReplace(expected,step.next.state);
            Check(revision==step.next.revision);
            if(step.retaining){
                Check(data.expected->ownership.retained_rows<OrchardOwnershipInventory::kMaxRows);
                bytes.retained.push_back(std::move(bytes.current));++data.expected->ownership.retained_rows;
            }
            bytes.current=std::move(step.next);
        }
    }
    // Reauthenticate the complete expected post-write catalog before committing.
    // No source lookup, scan restore, archive cause callback or proof runs here.
    {auto fresh=Owner::CaptureCatalog(w,session,data.profile);data.expected->Recheck(*fresh);}
    tx.Commit();data.next_account=index+1;data.usable=true;return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Read(WalletManager& w,uint64_t s,const Profile& p,const RestorePoint& point){
    std::unique_ptr<Owner::CapturedRead> captured;
    {
        auto lease=w.AcquireDatabaseLease();Check(lease->Session()==s&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        Owner owner(w,s,p,Owner::ExistingRead{});
        captured=owner.CaptureRead(p);tx.Commit();
    }
    // A standalone read holds no wallet/SQLite owner while restoring its scan.
    // Existing callers that retain an outer lease do not gain this guarantee.
    auto result=captured->Restore(p,point);
    {
        auto lease=w.AcquireDatabaseLease();Check(lease->Session()==s&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        Owner owner(w,s,p,Owner::ExistingRead{});
        owner.RecheckRead(*captured,result);tx.Commit();
    }
    return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::ReadForReplay(WalletManager& w,uint64_t s,const Profile& p,const RuntimeAccountReplay& view){
    const auto origin_event=view.Event(1);const auto& context=origin_event->context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    std::unique_ptr<Owner::CapturedRead> captured;
    {
        auto lease=w.AcquireDatabaseLease();Check(lease->Session()==s&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        Owner owner(w,s,p,Owner::ExistingRead{});
        captured=owner.CaptureRead(p);tx.Commit();
    }
    const auto receipt=OrchardAccountState::ReadDeliveryMetadata(captured->saved.state,p.domain,
        captured->viewing,p.activation,view.Point({}).checkpoint.block_hash);
    const auto point=view.Point({receipt.sequence,receipt.digest});
    auto result=captured->Restore(p,point);
    {
        auto lease=w.AcquireDatabaseLease();Check(lease->Session()==s&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        Owner owner(w,s,p,Owner::ExistingRead{});
        owner.RecheckRead(*captured,result);tx.Commit();
    }
    return result;
}
OrchardAccountDelivery::IssuedReceiver OrchardAccountDelivery::IssueReceiverForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,orchard::WalletScope scope){
    Check(scope==orchard::WalletScope::External||scope==orchard::WalletScope::Internal);
    // Transaction begins before identity, ciphertext or derivation reads.
    // The outer lease keeps SQLite alive until rollback on every failure.
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);
    Transaction tx(lease->Database());Owner owner(w,session,p,true);
    auto current=owner.RestoreReplay(p,view);
    auto issued=current.account.IssueReceiver(scope);
    IssuedReceiver result{0,issued.second.EncodeAddress(owner.identity.network)};
    result.revision=owner.Replace(current.revision,std::move(issued.first)).revision;
    tx.Commit();return result;
}
std::vector<OrchardAccountDelivery::Enrolled> OrchardAccountDelivery::Owner::ReadInventory(
        const RuntimeAccountReplay& view,bool allow_empty){
    auto& owner=*this;const auto origin_event=view.Event(1);const auto& context=origin_event->context;
    struct Statement {
        sqlite3_stmt* p=nullptr;
        ~Statement(){sqlite3_finalize(p);}
    } rows;
    Check(sqlite3_prepare_v2(owner.lease->Database(),
        "SELECT wallet_id,account,revision FROM orchard_wallet_snapshots ORDER BY account",-1,&rows.p,nullptr)==SQLITE_OK);
    using Locator=std::pair<orchard::Hash,uint32_t>;
    std::map<Locator,uint64_t> inventory;
    int rc;
    while((rc=sqlite3_step(rows.p))==SQLITE_ROW){
        Check(sqlite3_column_type(rows.p,0)==SQLITE_BLOB&&sqlite3_column_bytes(rows.p,0)==32);
        Check(sqlite3_column_type(rows.p,1)==SQLITE_INTEGER&&sqlite3_column_type(rows.p,2)==SQLITE_INTEGER);
        const auto number=sqlite3_column_int64(rows.p,1),revision=sqlite3_column_int64(rows.p,2);
        Check(number>=0&&number<0x80000000LL&&revision>0);
        orchard::Hash id{};std::copy_n(static_cast<const uint8_t*>(sqlite3_column_blob(rows.p,0)),32,id.begin());
        // Operational refusal before effects, never a truncated inventory.
        if(inventory.size()>=65536)throw std::runtime_error("Orchard account recovery inventory capacity exceeded");
        Check(inventory.emplace(Locator{id,uint32_t(number)},uint64_t(revision)).second);
    }
    Check(rc==SQLITE_DONE);
    std::vector<Enrolled> result;std::set<Locator> authenticated;
    for(const auto& [locator,revision]:inventory){
        if(locator.first!=owner.identity.wallet_id)continue;
        if(result.size()>=1024)throw std::runtime_error("Orchard account recovery capacity exceeded");
        auto identity=owner.identity;identity.account=locator.second;
        const auto keys=orchard::WalletKeys::FromSeed(owner.seed->Bytes(),identity.account);
        Owner::ViewingKey fvk(keys);
        orchard::WalletSnapshotStore store(owner.lease->Database(),identity,owner.seed->Bytes());
        const auto saved=store.Read();Check(saved&&saved->revision==revision);
        const auto receipt=OrchardAccountState::ReadDeliveryMetadata(saved->state,context.domain,fvk.bytes,
            context.activation_height,view.Point({}).checkpoint.block_hash);
        const auto point=view.Point({receipt.sequence,receipt.digest});
        auto account=OrchardAccountState::Restore(saved->state,context.domain,fvk.bytes,
            context.activation_height,point.checkpoint,point.lookups);
        Check(account.ParentSnapshotRevision()<saved->revision);
        const Profile profile{context.domain,context.activation_height,identity.account};
        ValidateBoundRequests(account.Operations(),identity,profile);
        owner.ValidateShieldHistories(account.Operations());
        Check(authenticated.insert(locator).second);
        // Archive records deliberately use derived wallet IDs in this SAME
        // snapshot table. Recognize only records reached from the restored
        // account's authenticated head; unrelated rows remain a hard refusal.
        std::vector<std::pair<orchard::Hash,uint64_t>> archive_revisions;
        OrchardOperationArchive archive(owner.lease->Database(),identity,context.domain,owner.seed->Bytes());
        const auto cursor=archive.Begin(account); // Preserve exact current account encoding/owner guard.
        const auto captured=archive.CaptureCurrent(fvk.bytes,context.activation_height);
        Check(captured.revision==saved->revision && captured.metadata.Archive()==account.Archive() &&
            captured.metadata.Delivery()==account.Delivery() &&
            captured.metadata.ParentSnapshotRevision()==account.ParentSnapshotRevision());
        Check(captured.archive.size()==cursor.Remaining()&&captured.archive.size()<=inventory.size());
        for(const auto& archived:captured.archive){
            const auto& record=archived.record;
            // Archive IDs differ; requests belong to the original account.
            ValidateBoundRequests(record.operation,identity,profile);
            owner.ValidateShieldHistories(record.operation);
            const Locator record_locator{archived.identity.wallet_id,archived.identity.account};
            const auto found=inventory.find(record_locator);
            Check(found!=inventory.end()&&found->second==record.revision&&authenticated.insert(record_locator).second);
            archive_revisions.emplace_back(record_locator.first,record.revision);
        }
        result.push_back({identity.account,{saved->revision,std::move(account)},std::move(archive_revisions)});
    }
    if(result.empty()&&!allow_empty)throw std::runtime_error("Wallet recovery account baseline reconciliation required");
    Check(authenticated.size()==inventory.size());
    return result;
}
void OrchardAccountDelivery::Owner::ValidateRetainedInventory(const std::vector<OrchardAccountDelivery::Enrolled>& accounts,const RuntimeAccountReplay& view){
    struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} table,rows;
    auto* db=lease->Database();
    Check(sqlite3_prepare_v2(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='orchard_wallet_retained'",-1,&table.p,nullptr)==SQLITE_OK);
    const int present=sqlite3_step(table.p);
    if(present==SQLITE_DONE){Check(catalog&&catalog->accounts.empty());return;}
    Check(present==SQLITE_ROW&&sqlite3_step(table.p)==SQLITE_DONE);
    Check(sqlite3_prepare_v2(db,"SELECT wallet_id,account,revision FROM orchard_wallet_retained ORDER BY wallet_id,account,revision",-1,&rows.p,nullptr)==SQLITE_OK);
    size_t count=0;int rc;
    while((rc=sqlite3_step(rows.p))==SQLITE_ROW){
        Check(++count<=65536&&sqlite3_column_type(rows.p,0)==SQLITE_BLOB&&sqlite3_column_bytes(rows.p,0)==32);
        Check(sqlite3_column_type(rows.p,1)==SQLITE_INTEGER&&sqlite3_column_type(rows.p,2)==SQLITE_INTEGER);
        const auto account=sqlite3_column_int64(rows.p,1),revision=sqlite3_column_int64(rows.p,2);
        Check(account>=0&&account<0x80000000LL&&revision>0);
        auto retained_identity=identity;retained_identity.account=static_cast<uint32_t>(account);
        const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(rows.p,0));Check(bytes);std::copy_n(bytes,32,retained_identity.wallet_id.begin());
        // Current inventory has already authenticated all normal and reached
        // archive owners. ReadRetained also requires a current authenticated
        // envelope and a strictly earlier revision; orphan history refuses.
        orchard::WalletSnapshotStore retained(db,retained_identity,seed->Bytes());
        (void)retained.ReadRetained(static_cast<uint64_t>(revision));
    }
    Check(rc==SQLITE_DONE);
    size_t predecessors=0;const auto origin_event=view.Event(1);const auto& context=origin_event->context;
    for(const auto& enrolled:accounts){
        auto account_identity=identity;account_identity.account=enrolled.number;
        const auto keys=orchard::WalletKeys::FromSeed(seed->Bytes(),enrolled.number);ViewingKey viewing(keys);
        orchard::WalletSnapshotStore snapshots(db,account_identity,seed->Bytes());
        auto revision=enrolled.state.account.ParentSnapshotRevision();auto upper=enrolled.state.revision;
        while(revision){
            Check(++predecessors<=65536&&revision<upper);const auto parent=snapshots.ReadRetained(revision);
            const auto receipt=OrchardAccountState::ReadDeliveryMetadata(parent.state,context.domain,viewing.bytes,context.activation_height,view.Point({}).checkpoint.block_hash);
            const auto point=view.Point({receipt.sequence,receipt.digest});
            const auto restored=OrchardAccountState::Restore(parent.state,context.domain,viewing.bytes,context.activation_height,point.checkpoint,point.lookups);
            ValidateBoundRequests(restored.Operations(),account_identity,
                {context.domain,context.activation_height,account_identity.account});
            ValidateShieldHistories(restored.Operations());
            upper=revision;revision=restored.ParentSnapshotRevision();
        }
    }
}
std::vector<OrchardAccountDelivery::Enrolled> OrchardAccountDelivery::ReadEnrolledForReplay(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view){
    const auto origin_event=view.Event(1);const auto& context=origin_event->context;
    const Profile profile{context.domain,context.activation_height,0};
    Owner owner(w,session,profile);Transaction tx(owner.lease->Database());
    auto result=owner.ReadInventory(view,false);tx.Commit();return result;
}
OrchardAccountDelivery::CatalogEnrolled OrchardAccountDelivery::ReadCatalogForReplay(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view){
    return ReadCatalogWithRestorePoints(w,session,view,[&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
OrchardAccountDelivery::CatalogEnrolled OrchardAccountDelivery::ReadCatalogWithRestorePoints(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    const auto event=view.Event(1);const auto& context=event->context;
    const Profile profile{context.domain,context.activation_height,0};
    std::unique_ptr<Owner::CapturedCatalog> captured;
    {
        auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        captured=Owner::CaptureCatalog(w,session,profile);tx.Commit();
    }
    // All declared current, reached and retained payloads belong to the
    // captured snapshot. The standalone API restores with no owners of its
    // own; composite callers must also release their surrounding leases.
    auto result=captured->Restore(points);
    {
        auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        auto fresh=Owner::CaptureCatalog(w,session,profile);
        captured->Recheck(*fresh);tx.Commit();
    }
    return result;
}
OrchardAccountDelivery::CatalogEnrolled OrchardAccountDelivery::ReadCatalogForReplayInTransaction(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view){
    const auto origin_event=view.Event(1);const auto& context=origin_event->context;
    const Profile profile{context.domain,context.activation_height,0};
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);
    Check(lease->Database()&&!sqlite3_get_autocommit(lease->Database()));
    std::optional<OrchardAccountCatalog::Snapshot> catalog;
    {
        auto seed=lease->CopyRecoverySeed(session);
        catalog=OrchardAccountCatalog::Read(lease->Database(),seed->Bytes());Check(catalog&&catalog->generated);
        (void)ExistingIdentity(lease->Database(),profile,!catalog->accounts.empty());
    }
    if(catalog->accounts.empty()){
        // Genuinely empty catalogs may predate storage schema. Do not construct
        // a WalletSnapshotStore or run DDL just to report known emptiness.
        const auto exists=[&](const char* name){
            struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
            Check(sqlite3_prepare_v2(lease->Database(),"SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",-1,&q.p,nullptr)==SQLITE_OK);
            Check(sqlite3_bind_text(q.p,1,name,-1,SQLITE_STATIC)==SQLITE_OK);const int rc=sqlite3_step(q.p);
            if(rc==SQLITE_DONE)return false;
            Check(rc==SQLITE_ROW&&sqlite3_step(q.p)==SQLITE_DONE);return true;
        };
        const bool schema=exists("orchard_wallet_schema"),rows=exists("orchard_wallet_snapshots"),retained=exists("orchard_wallet_retained");
        Check((!schema&&!rows&&!retained)||(schema&&rows&&retained));
        if(schema){
            struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} version;
            Check(sqlite3_prepare_v2(lease->Database(),"SELECT version FROM orchard_wallet_schema WHERE id=1",-1,&version.p,nullptr)==SQLITE_OK);
            Check(sqlite3_step(version.p)==SQLITE_ROW&&sqlite3_column_type(version.p,0)==SQLITE_INTEGER&&sqlite3_column_int64(version.p,0)==1&&sqlite3_step(version.p)==SQLITE_DONE);
            for(const auto* sql:{"SELECT 1 FROM orchard_wallet_snapshots","SELECT 1 FROM orchard_wallet_retained"}){
                Statement q;Check(sqlite3_prepare_v2(lease->Database(),sql,-1,&q.p,nullptr)==SQLITE_OK);Check(sqlite3_step(q.p)==SQLITE_DONE);
            }
        }
        CatalogEnrolled result{std::move(*catalog),{}};return result;
    }
    Owner owner(w,session,profile,true);auto accounts=owner.ValidateCatalogInventory(profile,view);
    Check(owner.catalog==catalog);CatalogEnrolled result{std::move(*catalog),std::move(accounts)};
    return result;
}
std::vector<OrchardAccountDelivery::Enrolled> OrchardAccountDelivery::Owner::ValidateCatalogInventory(
        const OrchardAccountDelivery::Profile& p,const RuntimeAccountReplay& view){
    const auto origin_event=view.Event(1);const auto& context=origin_event->context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    catalog=OrchardAccountCatalog::Read(lease->Database(),seed->Bytes());Check(catalog&&catalog->generated);
    // First genuine creation is between schema initialization and its first
    // retained write here. Keep that existing empty-catalog initialization
    // owner; established catalogs must have the complete read-only schema.
    std::optional<OrchardOwnershipInventory::Snapshot> ownership;
    if(!catalog->accounts.empty()) {
        ownership=OrchardOwnershipInventory::Read(lease->Database(),seed->Bytes());
        Check(ownership->catalog==*catalog&&ownership->wallet_id==identity.wallet_id);
        for(const auto& account:ownership->accounts)ValidateCatalogRequests(account);
    }
    auto inventory=ReadInventory(view,true);Check(inventory.size()==catalog->accounts.size());
    if(ownership) {
        Check(ownership->accounts.size()==inventory.size());
        for(size_t i=0;i<inventory.size();++i)
            Check(ownership->accounts[i].entry.account==inventory[i].number&&
                  ownership->accounts[i].current.revision==inventory[i].state.revision);
    }
    for(size_t i=0;i<inventory.size();++i){
        const auto& entry=catalog->accounts[i];
        Check(entry.account==inventory[i].number&&entry.network==p.domain.network_code&&entry.genesis==p.domain.genesis_wire&&
            entry.branch==p.domain.branch_id&&entry.activation==p.activation);
    }
    ValidateRetainedInventory(inventory,view);return inventory;
}
OrchardAccountDelivery::IssuedReceiver OrchardAccountDelivery::IssueCatalogReceiverForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,orchard::WalletScope scope){
    return IssueCatalogWithRestorePoints(w,session,p,view,scope,false,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
OrchardAccountDelivery::IssuedReceiver OrchardAccountDelivery::CreateAccountForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view){
    return IssueCatalogWithRestorePoints(w,session,p,view,orchard::WalletScope::External,true,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
OrchardAccountDelivery::IssuedReceiver OrchardAccountDelivery::IssueCatalogWithRestorePoints(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        orchard::WalletScope scope,bool creating,const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    Check(bool(points)&&p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0&&
        (scope==orchard::WalletScope::External||scope==orchard::WalletScope::Internal));
    const auto first=view.Event(1);const auto& context=first->context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    std::unique_ptr<Owner::CapturedCatalog> captured;
    std::unique_ptr<Owner::ViewingKey> viewing;
    orchard::WalletStorageIdentity identity;
    {
        auto lease=w.AcquireDatabaseLease();
        Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        captured=Owner::CaptureCatalog(w,session,p);
        // Derive only viewing authority while the existing seed is pinned.
        // No first-account schema or new identity is initialized by capture.
        auto seed=lease->CopyRecoverySeed(session);
        const auto keys=orchard::WalletKeys::FromSeed(seed->Bytes(),p.account);
        viewing=std::make_unique<Owner::ViewingKey>(keys);
        identity=ExistingIdentity(lease->Database(),p,false);tx.Commit();
    }
    const auto inventory=captured->Restore(points);
    const auto found=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),
        [&](const auto& entry){return entry.number==p.account;});
    Check(creating?found==inventory.accounts.end():found!=inventory.accounts.end());
    auto expected_catalog=inventory.catalog;
    if(creating){
        Check(scope==orchard::WalletScope::External&&expected_catalog.accounts.size()<OrchardAccountCatalog::kMaxAccounts&&
            expected_catalog.revision<uint64_t(INT64_MAX));
        expected_catalog.accounts.push_back({p.account,static_cast<uint8_t>(p.domain.network_code),
            p.domain.genesis_wire,p.domain.branch_id,p.activation});
        std::sort(expected_catalog.accounts.begin(),expected_catalog.accounts.end(),
            [](const auto& a,const auto& b){return a.account<b.account;});++expected_catalog.revision;
    }
    auto account=[&]{
        if(!creating)return found->state.account;
        const auto origin=points({});
        const auto initial=OrchardAccountState::Begin(p.domain,viewing->bytes,p.activation,origin.checkpoint.block_hash);
        return OrchardAccountState::Restore(initial.Encode(),p.domain,viewing->bytes,p.activation,origin.checkpoint,origin.lookups);
    }();
    auto issued=account.IssueReceiver(scope);auto encoded=issued.first.Encode();
    const auto expected_revision=creating?0:found->state.revision;
    Check(expected_revision<uint64_t(INT64_MAX));
    IssuedReceiver result{expected_revision+1,issued.second.EncodeAddress(identity.network)};
    const auto index=creating?size_t{0}:static_cast<size_t>(found-inventory.accounts.begin());
    if(!creating){
        auto& bytes=*captured->bytes[index];
        Check(captured->ownership.retained_rows<OrchardOwnershipInventory::kMaxRows);
        bytes.retained.reserve(bytes.retained.size()+1);
    }
    // No Point/Event/body/proof/account restoration callback occurs after
    // reacquiring the writer. The result remains private until checked COMMIT.
    auto lease=w.AcquireDatabaseLease();
    Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
    RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
    {auto fresh=Owner::CaptureCatalog(w,session,p);captured->Recheck(*fresh);}
    {
        Owner owner(w,session,p,true,creating);
        Check(Owner::CapturedCatalog::SameIdentity(identity,owner.identity)&&
            CRYPTO_memcmp(viewing->bytes.data(),owner.fvk.bytes.data(),viewing->bytes.size())==0);
        Check(owner.store.StageReplaceRetaining(expected_revision,encoded)==result.revision);
        if(creating){
            const OrchardAccountCatalog::Entry entry{p.account,static_cast<uint8_t>(p.domain.network_code),
                p.domain.genesis_wire,p.domain.branch_id,p.activation};
            Check(OrchardAccountCatalog::StageAppend(lease->Database(),owner.seed->Bytes(),inventory.catalog.revision,entry)==expected_catalog.revision);
        }
    }
    auto fresh=Owner::CaptureCatalog(w,session,p);
    if(creating){
        Check(fresh->ownership.catalog==expected_catalog&&fresh->ownership.current_rows==captured->ownership.current_rows+1&&
            fresh->ownership.retained_rows==captured->ownership.retained_rows&&fresh->bytes.size()==captured->bytes.size()+1);
        const auto row=std::find_if(fresh->ownership.accounts.begin(),fresh->ownership.accounts.end(),
            [&](const auto& entry){return entry.entry.account==p.account;});Check(row!=fresh->ownership.accounts.end());
        const auto added=static_cast<size_t>(row-fresh->ownership.accounts.begin());const auto& bytes=*fresh->bytes[added];
        Check(Owner::CapturedCatalog::SameIdentity(identity,row->identity)&&bytes.current.revision==1&&
            Owner::CapturedCatalog::Equal(bytes.current.state,encoded)&&bytes.retained.empty()&&bytes.archives.empty()&&
            CRYPTO_memcmp(viewing->bytes.data(),bytes.viewing.bytes.data(),viewing->bytes.size())==0);
        // Remove only the verified new owner from this temporary post-write
        // comparison, so every prior owner still requires exact byte equality.
        fresh->bytes.erase(fresh->bytes.begin()+added);fresh->ownership.accounts.erase(row);
        fresh->ownership.catalog=inventory.catalog;--fresh->ownership.current_rows;
    }else{
        auto& bytes=*captured->bytes[index];bytes.retained.push_back(std::move(bytes.current));
        bytes.current={result.revision,std::move(encoded)};++captured->ownership.retained_rows;
    }
    captured->Recheck(*fresh);tx.Commit();return result;
}
void OrchardAccountDelivery::Owner::ReconcileSpendCatalog(WalletManager& w,uint64_t session,
        const Profile& p,const RuntimeAccountReplay& view,const std::optional<orchard::Hash>& new_operation){
    std::vector<Enrolled> inventory;
    {
        Owner owner(w,session,p,true);inventory=owner.ValidateCatalogInventory(p,view);
    }
    const auto head=view.Head();Check(head.sequence&&!head.digest.IsNull());
    for(const auto& enrolled:inventory){
        const auto receipt=enrolled.state.account.Delivery();
        Check(receipt.sequence==head.sequence&&receipt.digest==head.digest);
    }
    for(const auto& enrolled:inventory){
        const Profile account_profile{p.domain,p.activation,enrolled.number};
        Owner owner(w,session,account_profile,true);
        auto current=owner.RestoreReplay(account_profile,view);
        Check(current.revision==enrolled.state.revision);
        if(new_operation){
            Check(!current.account.Operations().Entries().contains(*new_operation));
            OrchardOperationArchive archive(owner.lease->Database(),owner.identity,p.domain,owner.seed->Bytes());
            Check(!archive.Contains(*new_operation));
        }
        (void)owner.Reconcile(std::move(current),account_profile,view);
    }
}
std::unique_ptr<OrchardCatalogRecoveryPlan> OrchardAccountDelivery::PrepareOrdinaryPaymentForReplay(
        WalletManager& w,const WalletSigningIdentity& selected,const RuntimeAccountReplay& view){
    return PrepareOrdinaryPaymentWithPoints(w,selected,view,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
std::unique_ptr<OrchardCatalogRecoveryPlan> OrchardAccountDelivery::PrepareOrdinaryPaymentWithPoints(
        WalletManager& w,const WalletSigningIdentity& selected,const RuntimeAccountReplay& view,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    Check(!selected.name.empty()&&selected.session);
    {
        auto lease=w.AcquireDatabaseLease();
        Check(w.database_leases_==1&&lease->Database()&&lease->WalletName()==selected.name&&
              lease->Session()==selected.session&&sqlite3_get_autocommit(lease->Database()));
    }
    // Capture/authenticate under FULL SQL, release every wallet/seed owner,
    // then restore and compute actual archive-cause reactivation privately.
    auto plan=PrepareCatalogRecoveryWithPoints(w,selected.session,view,CatalogRecoveryAction::Reconcile,0,points);
    const auto head=view.Head();Check(head.sequence&&!head.digest.IsNull());
    for(const auto& enrolled:plan->data_->before.accounts){
        const auto receipt=enrolled.state.account.Delivery();
        Check(receipt.sequence==head.sequence&&receipt.digest==head.digest);
    }
    plan->data_->ordinary_payment=true;
    return plan;
}
SignResult OrchardAccountDelivery::SignAndStageOrdinaryForReplay(WalletManager& w,
        const WalletSigningIdentity& selected,const RuntimeAccountReplay& view,
        const UnsignedTransaction& input,const PendingPaymentIntent& intent){
    auto prepared=PrepareOrdinaryPaymentForReplay(w,selected,view);
    return CommitOrdinaryPaymentForReplay(w,selected,std::move(prepared),input,intent);
}
SignResult OrchardAccountDelivery::CommitOrdinaryPaymentForReplay(WalletManager& w,
        const WalletSigningIdentity& selected,std::unique_ptr<OrchardCatalogRecoveryPlan> prepared,
        const UnsignedTransaction& input,const PendingPaymentIntent& intent){
    Check(prepared&&prepared->data_&&prepared->data_->usable&&prepared->data_->ordinary_payment&&
          prepared->data_->next_account==0&&prepared->data_->session==selected.session);
    auto& data=*prepared->data_;data.usable=false;
    auto lease=w.AcquireDatabaseLease();
    Check(w.database_leases_==1&&lease->Database()&&!selected.name.empty()&&selected.session&&
          lease->WalletName()==selected.name&&lease->Session()==selected.session);
    RequireShieldFullSync(lease->Database());Transaction transaction(lease->Database());
    {auto fresh=Owner::CaptureCatalog(w,selected.session,data.profile);data.expected->Recheck(*fresh);}
    // All planned account reactivations and the ordinary payment share this
    // one transaction. No independent recovery commit can escape a later refusal.
    for(size_t i=0;i<data.accounts.size();++i){
        auto& planned=data.accounts[i];auto& bytes=*data.expected->bytes[i];
        const Profile profile{data.profile.domain,data.profile.activation,data.before.accounts[i].number};
        Owner owner(w,selected.session,profile,Owner::ExistingRead{});
        for(auto& step:planned.steps){
            const auto expected=bytes.current.revision;
            const auto revision=step.retaining?owner.store.StageReplaceRetaining(expected,step.next.state):
                owner.store.StageReplace(expected,step.next.state);
            Check(revision==step.next.revision);
            if(step.retaining){
                Check(data.expected->ownership.retained_rows<OrchardOwnershipInventory::kMaxRows);
                bytes.retained.push_back(std::move(bytes.current));++data.expected->ownership.retained_rows;
            }
            bytes.current=std::move(step.next);
        }
    }
    {auto fresh=Owner::CaptureCatalog(w,selected.session,data.profile);data.expected->Recheck(*fresh);}
    {
        auto pin=lease->CopyRecoverySeed(selected.session);
        auto inventory=OrchardOwnershipInventory::Read(lease->Database(),pin->Bytes());
        const auto ordinary=lease->ReadPendingPaymentsInTransaction(*pin);
        using Outpoint=std::pair<orchard::Hash,uint32_t>;
        std::set<Outpoint> reserved;
        for(const auto& current:inventory.current_inputs)
            Check(reserved.emplace(current.input.txid_wire,current.input.output_index).second);
        for(const auto& payment:ordinary)for(const auto& coin:payment.inputs){
            std::vector<uint8_t> bytes;Check(coin.txid.size()==64&&util::unhex(coin.txid,bytes)&&bytes.size()==32);
            orchard::Hash wire{};std::reverse_copy(bytes.begin(),bytes.end(),wire.begin());
            Check(reserved.emplace(wire,coin.vout).second);
        }
        for(const auto& txin:input.tx.vin){
            orchard::Hash wire{};const auto& hash=txin.prevout.txid.AsUint256();
            std::copy(hash.begin(),hash.end(),wire.begin());
            Check(!reserved.contains({wire,txin.prevout.vout}));
        }
    }
    auto signed_result=WalletTransactionOwner::Sign(w,selected,input,&intent,true,true);
    if(!signed_result.success)throw std::runtime_error(signed_result.error);
    // Signature/history preparation cannot silently change an Orchard owner.
    {auto fresh=Owner::CaptureCatalog(w,selected.session,data.profile);data.expected->Recheck(*fresh);}
    static_assert(std::is_nothrow_move_constructible_v<SignResult>);
    transaction.Commit();
    return signed_result;
}
std::unique_ptr<OrchardAccountDelivery::PreparedSpend> OrchardAccountDelivery::ReserveCatalogShieldForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
    return ReserveShieldForReplay(w,session,p,expected,view,id,inputs,payments,outputs,fee,nullptr).direct;
}
std::unique_ptr<OrchardAccountDelivery::QueuedSpend> OrchardAccountDelivery::QueueCatalogShieldRequestForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs){
    return PrepareCatalogShieldRequestForReplay(w,session,p,expected,view,id,inputs,payments,outputs,fee,jobs).Publish();
}
OrchardShieldRequest OrchardAccountDelivery::PrepareCatalogShieldRequestForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs){
    return ReserveShieldForReplay(w,session,p,expected,view,id,inputs,payments,outputs,fee,&jobs).queued;
}
std::unique_ptr<OrchardAccountDelivery::QueuedSpend> OrchardAccountDelivery::FindCatalogShieldRequestForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
    return ReserveShieldForReplay(w,session,p,0,view,id,inputs,payments,outputs,fee,nullptr,true).queued.result;
}
std::vector<orchard::ResolvedInput> OrchardAccountDelivery::ReadCatalogShieldCandidatesForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view){
    {auto lease=w.AcquireDatabaseLease();Check(w.database_leases_==1&&lease->Session()==session&&
        lease->Database()&&sqlite3_get_autocommit(lease->Database()));}
    auto prepared=PrepareCatalogRecovery(w,session,view,CatalogRecoveryAction::Observe);
    return ReadPreparedShieldCandidates(w,session,p,expected,*prepared);
}
std::vector<orchard::ResolvedInput> OrchardAccountDelivery::ReadPreparedShieldCandidates(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const OrchardCatalogRecoveryPlan& plan){
    Check(plan.data_&&plan.data_->usable&&plan.data_->session==session);
    const auto& data=*plan.data_;
    Check(data.profile.domain.network_code==p.domain.network_code&&data.profile.domain.genesis_wire==p.domain.genesis_wire&&
          data.profile.domain.branch_id==p.domain.branch_id&&data.profile.activation==p.activation);
    const auto found=std::find_if(data.before.accounts.begin(),data.before.accounts.end(),[&](const auto& a){return a.number==p.account;});
    Check(found!=data.before.accounts.end());
    CheckRequest(found->state.revision==expected,OrchardRequestError::Code::StaleAccountRevision);
    std::set<std::pair<orchard::Hash,uint32_t>> reserved;
    for(const auto& account:data.accounts){
        const auto receipt=account.result.account.Delivery();Check(receipt.sequence==data.head.sequence&&receipt.digest==data.head.digest);
        for(const auto& [id,entry]:account.result.account.Operations().Entries())for(const auto& input:entry.inputs)
            Check(reserved.emplace(input.txid_wire,input.output_index).second);
    }
    auto lease=w.AcquireDatabaseLease();Check(w.database_leases_==1&&lease->Session()==session&&lease->Database());
    RequireShieldFullSync(lease->Database());Transaction transaction(lease->Database());
    {auto fresh=Owner::CaptureCatalog(w,session,p);data.expected->Recheck(*fresh);}
    std::vector<orchard::ResolvedInput> result;
    {
        Owner owner(w,session,p,Owner::ExistingRead{});
        for(const auto& coin:lease->ReadShieldCandidatesInTransaction(*owner.seed)){
            std::vector<uint8_t> hash;Check(util::unhex(coin.txid,hash)&&hash.size()==32);
            orchard::Hash wire{};std::reverse_copy(hash.begin(),hash.end(),wire.begin());
            if(reserved.count({wire,coin.vout}))continue;
            result.push_back({wire,coin.vout,UINT32_MAX,coin.amount_una,coin.script});
        }
    }
    transaction.Commit();return result;
}
std::unique_ptr<OrchardAccountDelivery::QueuedSpend> OrchardAccountDelivery::FindStoredShieldRequestForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,uint64_t fee){
    auto prepared=PrepareShieldReservationForReplay(w,session,p,0,view,id,{},payments,{},fee,
        ShieldRequestMode::Stored,true);
    return TakePreparedShieldRetry(w,session,*prepared);
}
OrchardAccountDelivery::ShieldResult OrchardAccountDelivery::ReserveShieldForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs* jobs,bool existing_only){
    auto prepared=PrepareShieldReservationForReplay(w,session,p,expected,view,id,inputs,payments,outputs,fee,
        jobs||existing_only?ShieldRequestMode::Exact:ShieldRequestMode::Unbound,existing_only);
    if(existing_only){ShieldResult result;result.queued.result=TakePreparedShieldRetry(w,session,*prepared);return result;}
    return CommitShieldReservation(w,session,std::move(prepared),inputs,outputs,jobs);
}
std::unique_ptr<OrchardCatalogRecoveryPlan> OrchardAccountDelivery::PrepareShieldReservationForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,std::span<const orchard::TransparentOutput> outputs,
        uint64_t fee,ShieldRequestMode mode,bool existing_only){
    return PrepareShieldReservationWithPoints(w,session,p,expected,view,id,inputs,payments,outputs,fee,mode,existing_only,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
std::unique_ptr<OrchardCatalogRecoveryPlan> OrchardAccountDelivery::PrepareShieldReservationWithPoints(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,std::span<const orchard::TransparentOutput> outputs,
        uint64_t fee,ShieldRequestMode mode,bool existing_only,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    Check(id!=orchard::Hash{}&&!payments.empty()&&payments.size()<=DINERO_ORCHARD_V1_MAX_ACTIONS&&fee<=orchard::kMaxMoneyUna);
    Check(mode!=ShieldRequestMode::Stored||(inputs.empty()&&outputs.empty()));
    Check(mode==ShieldRequestMode::Stored||(!inputs.empty()&&inputs.size()<=1024&&outputs.size()<=1024));
    uint64_t deposit=0;for(const auto& payment:payments){
        Check(payment.amount_una>0&&payment.amount_una<=orchard::kMaxMoneyUna-deposit);deposit+=payment.amount_una;
    }
    {auto lease=w.AcquireDatabaseLease();Check(w.database_leases_==1&&lease->Session()==session&&
        lease->Database()&&sqlite3_get_autocommit(lease->Database()));}
    auto prepared=PrepareCatalogRecoveryWithPoints(w,session,view,CatalogRecoveryAction::Observe,0,points);
    auto& data=*prepared->data_;
    Check(data.profile.domain.network_code==p.domain.network_code&&data.profile.domain.genesis_wire==p.domain.genesis_wire&&
          data.profile.domain.branch_id==p.domain.branch_id&&data.profile.activation==p.activation);
    const auto found=std::find_if(data.before.accounts.begin(),data.before.accounts.end(),[&](const auto& a){return a.number==p.account;});
    Check(found!=data.before.accounts.end());
    const size_t index=static_cast<size_t>(found-data.before.accounts.begin());
    auto details=std::make_unique<OrchardCatalogRecoveryPlan::Data::Shield>();
    details->profile=p;details->id=id;details->fee=fee;details->deposit=deposit;
    details->payments=std::vector<orchard::WalletPayment>(payments.begin(),payments.end());details->bound=mode!=ShieldRequestMode::Unbound;
    details->existing_only=existing_only;details->index=index;
    const auto& identity=data.expected->ownership.accounts[index].identity;
    if(mode==ShieldRequestMode::Exact)details->exact_request=ShieldRequestCommitment(identity,p,id,inputs,payments,outputs,fee);
    if(details->bound){
        for(size_t i=0;i<data.before.accounts.size();++i){
            const auto& enrolled=data.before.accounts[i];const auto& account=enrolled.state.account;
            const auto pending=account.Operations().Entries().find(id);
            const auto& archive=data.expected->ownership.accounts[i].current.archive;
            const auto archived=std::find_if(archive.begin(),archive.end(),[&](const auto& record){return record.id==id;});
            if(pending==account.Operations().Entries().end()&&archived==archive.end())continue;
            CheckRequest(enrolled.number==p.account,OrchardRequestError::Code::RequestIdConflict);
            Check(!details->existing);
            const bool only_archive=pending==account.Operations().Entries().end();
            const auto& entry=only_archive?archived->record.operation.Entries().at(id):pending->second;
            auto request=details->exact_request;
            std::vector<orchard::TransparentOutput> selected_outputs(outputs.begin(),outputs.end());
            if(mode==ShieldRequestMode::Stored){
                Check(entry.shield_request.has_value());
                CheckRequest(entry.shield_request->fee_una==fee,OrchardRequestError::Code::RequestIdConflict);
                selected_outputs=entry.shield_request->outputs;
                request=ShieldRequestCommitment(identity,p,id,entry.inputs,payments,selected_outputs,fee);
            }
            Check(request.has_value());
            CheckRequest(entry.request_commitment==request,OrchardRequestError::Code::RequestIdConflict);
            if(archived!=archive.end()){
                const auto& retained=archived->record.operation.Entries().at(id);
                Check(retained.request_commitment==request);
                if(mode==ShieldRequestMode::Stored)Check(retained.shield_request.has_value());
            }
            details->existing=std::make_unique<QueuedSpend>(QueuedSpend{enrolled.state.revision,id,std::move(selected_outputs),fee,false});
            details->existing->existing_request=true;details->existing->archived=only_archive;details->existing->durable=entry;
            if(only_archive)details->existing->observation=archived->record.observation;
            else if(const auto observed=account.Observations().find(id);observed!=account.Observations().end())
                details->existing->observation=observed->second;
        }
    }
    const bool retry=bool(details->existing);data.shield=std::move(details);
    // Exact retries precede availability and reconciliation. They do not issue
    // change, generate a plan, inspect an executor or release reservations.
    if(retry||existing_only)return prepared;
    CheckRequest(found->state.revision==expected,OrchardRequestError::Code::StaleAccountRevision);
    Check(data.head.sequence&&!data.head.digest.IsNull());
    for(size_t i=0;i<data.before.accounts.size();++i){
        const auto& account=data.before.accounts[i].state.account;const auto receipt=account.Delivery();
        Check(receipt.sequence==data.head.sequence&&receipt.digest==data.head.digest&&!account.Operations().Entries().contains(id));
        const auto& archive=data.expected->ownership.accounts[i].current.archive;
        Check(std::none_of(archive.begin(),archive.end(),[&](const auto& record){return record.id==id;}));
    }
    CompleteCatalogRecoveryWithPoints(*prepared,CatalogRecoveryAction::Reconcile,0,points);
    return prepared;
}
std::unique_ptr<OrchardAccountDelivery::QueuedSpend> OrchardAccountDelivery::TakePreparedShieldRetry(
        WalletManager& w,uint64_t session,OrchardCatalogRecoveryPlan& prepared){
    Check(prepared.data_&&prepared.data_->usable&&prepared.data_->session==session&&prepared.data_->shield);
    auto& data=*prepared.data_;auto& shield=*data.shield;
    // Even an absent find-only result must recheck its complete capture before
    // returning, because callers use that absence to choose their next action.
    if(!shield.existing&&!shield.existing_only)return {};
    data.usable=false;
    auto lease=w.AcquireDatabaseLease();Check(w.database_leases_==1&&lease->Session()==session&&lease->Database());
    RequireShieldFullSync(lease->Database());Transaction transaction(lease->Database());
    {auto fresh=Owner::CaptureCatalog(w,session,shield.profile);data.expected->Recheck(*fresh);}
    transaction.Commit();return std::move(shield.existing);
}
OrchardAccountDelivery::ShieldResult OrchardAccountDelivery::CommitShieldReservation(
        WalletManager& w,uint64_t session,std::unique_ptr<OrchardCatalogRecoveryPlan> prepared,
        std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::TransparentOutput> outputs,OrchardProofJobs* jobs){
    Check(prepared&&prepared->data_&&prepared->data_->usable&&prepared->data_->session==session&&prepared->data_->shield);
    auto& data=*prepared->data_;auto& shield=*data.shield;
    if(shield.existing){ShieldResult result;result.queued.result=TakePreparedShieldRetry(w,session,*prepared);return result;}
    Check(!shield.existing_only&&!inputs.empty()&&inputs.size()<=1024&&outputs.size()<=1024&&shield.bound==bool(jobs));
    const auto& p=shield.profile;const auto& id=shield.id;const auto& payments=shield.payments;const auto fee=shield.fee;
    auto current=data.accounts[shield.index].result;
    std::optional<orchard::Hash> request;
    if(shield.bound){
        request=ShieldRequestCommitment(data.expected->ownership.accounts[shield.index].identity,p,id,inputs,payments,outputs,fee);
        if(shield.exact_request)Check(request==shield.exact_request);
    }
    std::set<std::pair<orchard::Hash,uint32_t>> reserved;
    for(const auto& account:data.accounts)for(const auto& [operation,entry]:account.result.account.Operations().Entries())
        for(const auto& input:entry.inputs)Check(reserved.emplace(input.txid_wire,input.output_index).second);
    data.usable=false;ShieldResult result;
    auto lease=w.AcquireDatabaseLease();Check(w.database_leases_==1&&lease->Database()&&lease->Session()==session);
    RequireShieldFullSync(lease->Database());Transaction transaction(lease->Database());
    {auto fresh=Owner::CaptureCatalog(w,session,p);data.expected->Recheck(*fresh);}
    // All planned archive reactivations and the new shield reservation belong
    // to this one writer. No partial reconciliation can escape a later failure.
    for(size_t i=0;i<data.accounts.size();++i){
        auto& bytes=*data.expected->bytes[i];const Profile profile{p.domain,p.activation,data.before.accounts[i].number};
        Owner owner(w,session,profile,Owner::ExistingRead{});
        for(auto& step:data.accounts[i].steps){
            const auto revision=step.retaining?owner.store.StageReplaceRetaining(bytes.current.revision,step.next.state):
                owner.store.StageReplace(bytes.current.revision,step.next.state);
            Check(revision==step.next.revision);
            if(step.retaining){Check(data.expected->ownership.retained_rows<OrchardOwnershipInventory::kMaxRows);
                bytes.retained.push_back(std::move(bytes.current));++data.expected->ownership.retained_rows;}
            bytes.current=std::move(step.next);
        }
    }
    {auto fresh=Owner::CaptureCatalog(w,session,p);data.expected->Recheck(*fresh);}
    {
        Owner owner(w,session,p,Owner::ExistingRead{});
        for(const auto& payment:lease->ReadPendingPaymentsInTransaction(*owner.seed))for(const auto& input:payment.inputs){
            std::vector<uint8_t> bytes;Check(input.txid.size()==64&&util::unhex(input.txid,bytes)&&bytes.size()==32);
            orchard::Hash wire{};std::reverse_copy(bytes.begin(),bytes.end(),wire.begin());
            Check(reserved.emplace(wire,input.vout).second);
        }
        std::vector<PendingPaymentInput> wallet_inputs;wallet_inputs.reserve(inputs.size());
        for(const auto& input:inputs){
            Check(input.sequence==UINT32_MAX&&reserved.emplace(input.txid_wire,input.output_index).second);
            uint256 txid;std::copy(input.txid_wire.begin(),input.txid_wire.end(),txid.begin());
            wallet_inputs.push_back({txid.GetHex(),input.output_index,input.amount_una,input.script_pub_key});
        }
        lease->ValidateUnreservedInputsInTransaction(*owner.seed,wallet_inputs);
        for(const auto& output:outputs)
            Check(bool(lease->ResolveSigningKeyInTransaction(util::hex(output.script_pub_key),*owner.seed)));
        std::vector<orchard::ResolvedInput> resolved(inputs.begin(),inputs.end());
        std::vector<orchard::TransparentOutput> transparent(outputs.begin(),outputs.end());
        auto signing=orchard::SigningContext::Create(p.domain,0,resolved,transparent,fee);
        Check(signing.RequiredValueBalance()==-static_cast<int64_t>(shield.deposit));
        auto plan=orchard::WalletBundlePlan::PrepareShield(owner.keys,payments);
        auto recovery=request?std::make_shared<const orchard::WalletStateBytes>(plan.ExportRecovery()):nullptr;
        const auto intent=plan.Intent(signing);Check(!intent.Nullifiers().empty()&&intent.Inputs().size()==inputs.size());
        std::unique_ptr<OrchardProofJobs::Submission> submission;
        if(jobs)submission=jobs->PrepareOwned(id,{owner.identity,p.domain.branch_id,session,lease->InstanceToken()},
            std::move(plan),std::move(signing));
        auto staged=owner.Replace(current.revision,request?
            current.account.ReserveShieldRequest(id,intent,*request,StoreShieldRequest(owner.identity.network,payments,outputs,fee),std::move(recovery)):
            current.account.Reserve(id,intent));
        auto& bytes=*data.expected->bytes[shield.index];
        Check(data.expected->ownership.retained_rows<OrchardOwnershipInventory::kMaxRows);
        bytes.retained.push_back(std::move(bytes.current));++data.expected->ownership.retained_rows;
        bytes.current={staged.revision,staged.account.Encode()};
        if(submission){
            submission->Bind(staged.account.Operations());
            result.queued.result=std::make_unique<QueuedSpend>(QueuedSpend{staged.revision,id,std::move(transparent),fee,false});
            result.queued.result->durable=staged.account.Operations().Entries().at(id);
        }else result.direct=std::make_unique<PreparedSpend>(PreparedSpend{staged.revision,id,
            staged.account.Operations(),std::move(plan),std::move(signing),std::move(transparent),fee});
        result.queued.submission=std::move(submission);
    }
    {auto fresh=Owner::CaptureCatalog(w,session,p);data.expected->Recheck(*fresh);}
    static_assert(std::is_nothrow_move_constructible_v<ShieldResult>);
    transaction.Commit();return result;
}
OrchardAccountDelivery::PreparedSpend OrchardAccountDelivery::ReserveCatalogSpendForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
    auto result=ReserveSpendForReplay(w,session,p,expected,view,id,payments,outputs,fee,nullptr);
    return std::move(*result.direct);
}
std::unique_ptr<OrchardAccountDelivery::QueuedSpend> OrchardAccountDelivery::QueueCatalogSpendForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs){
    return ReserveSpendForReplay(w,session,p,expected,view,id,payments,outputs,fee,&jobs).queued;
}
std::unique_ptr<OrchardAccountDelivery::QueuedSpend> OrchardAccountDelivery::QueueCatalogRequestForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs){
    return ReserveSpendForReplay(w,session,p,expected,view,id,payments,outputs,fee,&jobs,true).queued;
}
OrchardAccountDelivery::SpendResult OrchardAccountDelivery::ReserveSpendForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs* jobs,bool bind_request){
    return ReserveSpendWithRestorePoints(w,session,p,expected,view,id,payments,outputs,fee,jobs,bind_request,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
OrchardAccountDelivery::SpendResult OrchardAccountDelivery::ReserveSpendWithRestorePoints(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs* jobs,bool bind_request,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    Check(id!=orchard::Hash{}&&(!payments.empty()||!outputs.empty())&&payments.size()<=DINERO_ORCHARD_V1_MAX_ACTIONS);
    Check(fee<=orchard::kMaxMoneyUna);uint64_t required=fee;
    const auto add=[&](uint64_t amount){Check(amount>0&&amount<=orchard::kMaxMoneyUna-required);required+=amount;};
    for(const auto& payment:payments)add(payment.amount_una);
    for(const auto& output:outputs)add(output.amount_una);
    {
        auto lease=w.AcquireDatabaseLease();
        Check(w.database_leases_==1&&lease->Session()==session&&lease->Database()&&
              sqlite3_get_autocommit(lease->Database()));
    }
    // Authenticate and restore every account before acquiring the writer. The
    // observed capture also owns every reached archive, retained row and key.
    auto prepared=PrepareCatalogRecoveryWithPoints(w,session,view,CatalogRecoveryAction::Observe,0,points);
    auto& data=*prepared->data_;
    Check(data.profile.domain.network_code==p.domain.network_code&&data.profile.domain.genesis_wire==p.domain.genesis_wire&&
          data.profile.domain.branch_id==p.domain.branch_id&&data.profile.activation==p.activation);
    const auto requested=std::find_if(data.before.accounts.begin(),data.before.accounts.end(),
        [&](const auto& entry){return entry.number==p.account;});
    Check(requested!=data.before.accounts.end());
    const size_t index=static_cast<size_t>(requested-data.before.accounts.begin());
    std::optional<orchard::Hash> request;SpendResult result;
    if(bind_request){
        Check(jobs);
        request=SpendRequestCommitment(data.expected->ownership.accounts[index].identity,p,id,payments,outputs,fee);
        for(size_t i=0;i<data.before.accounts.size();++i){
            const auto& enrolled=data.before.accounts[i];const auto& account=enrolled.state.account;
            const auto pending=account.Operations().Entries().find(id);
            const auto& archive=data.expected->ownership.accounts[i].current.archive;
            const auto archived=std::find_if(archive.begin(),archive.end(),[&](const auto& record){return record.id==id;});
            if(pending==account.Operations().Entries().end()&&archived==archive.end())continue;
            CheckRequest(enrolled.number==p.account,OrchardRequestError::Code::RequestIdConflict);
            Check(!result.queued);
            if(archived!=archive.end())CheckRequest(archived->record.operation.Entries().at(id).request_commitment==request,OrchardRequestError::Code::RequestIdConflict);
            const bool only_archive=pending==account.Operations().Entries().end();
            const auto& entry=only_archive?archived->record.operation.Entries().at(id):pending->second;
            CheckRequest(entry.request_commitment==request,OrchardRequestError::Code::RequestIdConflict);
            result.queued=std::make_unique<QueuedSpend>(QueuedSpend{enrolled.state.revision,id,
                std::vector<orchard::TransparentOutput>(outputs.begin(),outputs.end()),fee,false});
            result.queued->existing_request=true;result.queued->archived=only_archive;result.queued->durable=entry;
            if(only_archive)result.queued->observation=archived->record.observation;
            else if(const auto observed=account.Observations().find(id);observed!=account.Observations().end())
                result.queued->observation=observed->second;
        }
        if(result.queued){
            // Exact retry: no reconciliation, random plan, executor lookup,
            // re-enqueue, SQL mutation or implicit cancellation.
            auto lease=w.AcquireDatabaseLease();
            Check(w.database_leases_==1&&lease->Session()==session&&lease->Database());
            RequireShieldFullSync(lease->Database());Transaction transaction(lease->Database());
            {auto fresh=Owner::CaptureCatalog(w,session,p);data.expected->Recheck(*fresh);}
            transaction.Commit();return result;
        }
    }
    CheckRequest(requested->state.revision==expected,OrchardRequestError::Code::StaleAccountRevision);
    const auto head=view.Head();Check(head.sequence&&!head.digest.IsNull());
    for(size_t i=0;i<data.before.accounts.size();++i){
        const auto& account=data.before.accounts[i].state.account;const auto receipt=account.Delivery();
        Check(receipt.sequence==head.sequence&&receipt.digest==head.digest&&!account.Operations().Entries().contains(id));
        const auto& archive=data.expected->ownership.accounts[i].current.archive;
        Check(std::none_of(archive.begin(),archive.end(),[&](const auto& record){return record.id==id;}));
    }
    // Reuse the same authenticated capture: no second snapshot can replace a
    // missing owner or silently change the request/revision checked above.
    CompleteCatalogRecoveryWithPoints(*prepared,CatalogRecoveryAction::Reconcile,0,points);
    auto current=data.accounts[index].result;
    std::set<orchard::Hash> reserved;std::set<orchard::Hash> operations;
    for(const auto& account:data.accounts)for(const auto& [operation,entry]:account.result.account.Operations().Entries()){
        Check(operations.insert(operation).second);
        for(const auto& nullifier:entry.nullifiers)Check(reserved.insert(nullifier).second);
    }
    struct Candidate{const ScannedOrchardNote* note;orchard::Hash nullifier;};std::vector<Candidate> candidates;
    for(const auto& note:current.account.Scan().Notes()){
        Check(note.note&&note.witness);orchard::Hash nullifier{};
        std::copy_n(note.note->Facts().nullifier,nullifier.size(),nullifier.begin());
        if(!reserved.contains(nullifier))candidates.push_back({&note,nullifier});
    }
    // Bounded action count: prefer larger existing notes, with a stable tie
    // break. An insufficient bounded selection refuses; no partial reservation.
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){
        const auto av=a.note->note->Facts().amount,bv=b.note->note->Facts().amount;
        return av!=bv?av>bv:a.nullifier<b.nullifier;
    });
    uint64_t selected=0;std::vector<orchard::WalletSpendInput> inputs;
    for(const auto& candidate:candidates){
        if(selected>=required||inputs.size()==DINERO_ORCHARD_V1_MAX_ACTIONS)break;
        const auto amount=candidate.note->note->Facts().amount;
        Check(amount<=orchard::kMaxMoneyUna-selected);selected+=amount;
        inputs.push_back({*candidate.note->note,*candidate.note->witness});
    }
    Check(selected>=required&&!inputs.empty());
    std::vector<orchard::WalletPayment> recipients(payments.begin(),payments.end());
    auto account=current.account;
    if(selected>required){
        Check(recipients.size()<DINERO_ORCHARD_V1_MAX_ACTIONS);
        auto issued=account.IssueReceiver(orchard::WalletScope::Internal);account=std::move(issued.first);
        recipients.push_back({selected-required,std::move(issued.second)});
    }
    orchard::Hash anchor{};const auto& checkpoint=current.account.Scan().Checkpoint();
    Check(checkpoint.height>=p.activation);std::copy(checkpoint.anchor.begin(),checkpoint.anchor.end(),anchor.begin());
    std::vector<orchard::TransparentOutput> transparent(outputs.begin(),outputs.end());
    auto signing=orchard::SigningContext::Create(p.domain,0,{},transparent,fee);
    std::unique_ptr<OrchardProofJobs::Submission> submission;
    data.usable=false;
    {
        auto lease=w.AcquireDatabaseLease();
        Check(w.database_leases_==1&&lease->Session()==session&&lease->Database());
        RequireShieldFullSync(lease->Database());Transaction transaction(lease->Database());
        {auto fresh=Owner::CaptureCatalog(w,session,p);data.expected->Recheck(*fresh);}
        // All archive reactivations and the new reservation share one FULL
        // transaction. A later key, capacity, SQL or commit refusal rolls back all.
        for(size_t i=0;i<data.accounts.size();++i){
            auto& planned=data.accounts[i];auto& bytes=*data.expected->bytes[i];
            const Profile profile{p.domain,p.activation,data.before.accounts[i].number};
            Owner owner(w,session,profile,Owner::ExistingRead{});
            for(auto& step:planned.steps){
                const auto revision=step.retaining?owner.store.StageReplaceRetaining(bytes.current.revision,step.next.state):
                    owner.store.StageReplace(bytes.current.revision,step.next.state);
                Check(revision==step.next.revision);
                if(step.retaining){
                    Check(data.expected->ownership.retained_rows<OrchardOwnershipInventory::kMaxRows);
                    bytes.retained.push_back(std::move(bytes.current));++data.expected->ownership.retained_rows;
                }
                bytes.current=std::move(step.next);
            }
        }
        {auto fresh=Owner::CaptureCatalog(w,session,p);data.expected->Recheck(*fresh);}
        {
            Owner owner(w,session,p,Owner::ExistingRead{});
            auto plan=orchard::WalletBundlePlan::PrepareSpend(owner.keys,inputs,anchor,recipients);
            auto recovery=request?std::make_shared<const orchard::WalletStateBytes>(plan.ExportRecovery()):nullptr;
            const auto intent=plan.Intent(signing);Check(intent.Inputs().empty());
            for(const auto& nullifier:intent.Nullifiers())Check(!reserved.contains(nullifier));
            if(jobs)submission=jobs->PrepareOwned(id,
                {owner.identity,p.domain.branch_id,session,lease->InstanceToken()},std::move(plan),std::move(signing));
            auto staged=owner.Replace(current.revision,request?
                account.ReserveSpendRequest(id,intent,*request,StoreShieldRequest(owner.identity.network,payments,outputs,fee),std::move(recovery)):
                account.Reserve(id,intent));
            auto& bytes=*data.expected->bytes[index];
            Check(data.expected->ownership.retained_rows<OrchardOwnershipInventory::kMaxRows);
            bytes.retained.push_back(std::move(bytes.current));++data.expected->ownership.retained_rows;
            bytes.current={staged.revision,staged.account.Encode()};
            if(submission){
                submission->Bind(staged.account.Operations());
                result.queued=std::make_unique<QueuedSpend>(QueuedSpend{staged.revision,id,std::move(transparent),fee,false});
                if(request)result.queued->durable=staged.account.Operations().Entries().at(id);
            }else result.direct=std::make_unique<PreparedSpend>(PreparedSpend{staged.revision,id,
                staged.account.Operations(),std::move(plan),std::move(signing),std::move(transparent),fee});
        }
        {auto fresh=Owner::CaptureCatalog(w,session,p);data.expected->Recheck(*fresh);}
        static_assert(std::is_nothrow_move_constructible_v<SpendResult>);
        transaction.Commit();
    }
    // No SQLite transaction, wallet lease or recovery seed survives publication.
    if(submission)result.queued->enqueued=submission->Publish();
    return result;
}
OrchardAccountDelivery::OwnedProof OrchardAccountDelivery::ReadCatalogProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,OrchardProofJobs& jobs){
    auto result=ReadCatalogProofWithRestorePoints(w,session,p,view,CatalogProofRequest::Any,
        id,{},{},{},0,jobs,[&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
    return {result.revision,result.state,std::move(result.proof)};
}
OrchardAccountDelivery::RequestProof OrchardAccountDelivery::ReadCatalogRequestProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs,bool resume_missing){
    return ReadCatalogProofWithRestorePoints(w,session,p,view,CatalogProofRequest::Spend,
        id,{},payments,outputs,fee,jobs,[&view](RuntimeOutboxCursor cursor){return view.Point(cursor);},resume_missing);
}
OrchardAccountDelivery::RequestProof OrchardAccountDelivery::ReadStoredCatalogRequestProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,bool shield,OrchardProofJobs& jobs,bool resume_missing){
    return ReadCatalogProofWithRestorePoints(w,session,p,view,
        shield?CatalogProofRequest::StoredShield:CatalogProofRequest::StoredSpend,
        id,{},{},{},0,jobs,[&view](RuntimeOutboxCursor cursor){return view.Point(cursor);},resume_missing);
}
OrchardAccountDelivery::RequestProof OrchardAccountDelivery::ReadCatalogShieldRequestProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs,bool resume_missing){
    return ReadCatalogProofWithRestorePoints(w,session,p,view,CatalogProofRequest::Shield,
        id,inputs,payments,outputs,fee,jobs,[&view](RuntimeOutboxCursor cursor){return view.Point(cursor);},resume_missing);
}
OrchardAccountDelivery::RequestProof OrchardAccountDelivery::ReadCatalogProofWithRestorePoints(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        CatalogProofRequest kind,const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
        std::span<const orchard::WalletPayment> payments,std::span<const orchard::TransparentOutput> outputs,
        uint64_t fee,OrchardProofJobs& jobs,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points,bool resume_missing){
    Check(!resume_missing||kind!=CatalogProofRequest::Any);
    Check(bool(points)&&id!=orchard::Hash{}&&p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
    Check(kind==CatalogProofRequest::Any||kind==CatalogProofRequest::Spend||kind==CatalogProofRequest::Shield||
        kind==CatalogProofRequest::StoredSpend||kind==CatalogProofRequest::StoredShield);
    if(kind==CatalogProofRequest::Spend)Check(!payments.empty()||!outputs.empty());
    if(kind==CatalogProofRequest::Shield)Check(!inputs.empty()&&!payments.empty());
    const auto event=view.Event(1);const auto& context=event->context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    std::unique_ptr<Owner::CapturedCatalog> captured;
    {
        auto lease=w.AcquireDatabaseLease();
        Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        captured=Owner::CaptureCatalog(w,session,p);tx.Commit();
    }
    // Authenticate and restore the complete captured catalog with wallet,
    // seed and SQLite ownership released before inspecting executor state.
    const auto inventory=captured->Restore(points);
    const auto requested=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),
        [&](const auto& account){return account.number==p.account;});
    Check(requested!=inventory.accounts.end());
    const auto index=static_cast<size_t>(requested-inventory.accounts.begin());
    const auto& identity=captured->ownership.accounts[index].identity;
    const auto& queue=requested->state.account.Operations();const auto operation=queue.Entries().find(id);
    CheckRequest(operation!=queue.Entries().end(),OrchardRequestError::Code::RequestNotCurrent);
    if(kind==CatalogProofRequest::Spend){
        Check(operation->second.inputs.empty());
        CheckRequest(operation->second.request_commitment==SpendRequestCommitment(identity,p,id,payments,outputs,fee),
            OrchardRequestError::Code::RequestIdConflict);
    }else if(kind==CatalogProofRequest::Shield){
        CheckRequest(operation->second.request_commitment==ShieldRequestCommitment(identity,p,id,inputs,payments,outputs,fee),
            OrchardRequestError::Code::RequestIdConflict);
    }
    // Restore authenticated all current and retained detail/digest bindings.
    // Requiring the matching details here distinguishes spend from shield and
    // refuses older ID-only records; an ID alone never invents payment intent.
    if(kind==CatalogProofRequest::StoredSpend)
        Check(operation->second.spend_request.has_value()&&!operation->second.shield_request&&operation->second.inputs.empty());
    if(kind==CatalogProofRequest::StoredShield)
        Check(operation->second.shield_request.has_value()&&!operation->second.spend_request&&!operation->second.inputs.empty());
    const auto& entry=operation->second;
    RequestProof result{requested->state.revision,entry,{},{}};
    static_assert(std::is_nothrow_move_constructible_v<RequestProof>);
    // This is an as-of source check, not lasting chain readiness. Authorization
    // and admission still capture fresh selected state before exposing Ready.
    const auto head=view.Head();
    const bool eligible=resume_missing&&entry.phase==OrchardOperationQueue::Phase::Reserved&&
        !requested->state.account.Observations().contains(id);
    std::unique_ptr<orchard::WalletKeys> recovery_keys;
    {
        auto lease=w.AcquireDatabaseLease();
        Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        auto fresh=Owner::CaptureCatalog(w,session,p);captured->Recheck(*fresh);
        auto proof=jobs.CaptureOwned(id,{identity,p.domain.branch_id,session,lease->InstanceToken()},queue);
        result.state=proof.state;result.proof=std::move(proof.proof);
        if(eligible&&!result.state){
            Check(entry.recovery&&entry.request_commitment&&
                (entry.shield_request.has_value()!=entry.spend_request.has_value()));
            Owner owner(w,session,p,Owner::ExistingRead{});
            Check(Owner::CapturedCatalog::SameIdentity(owner.identity,identity));
            recovery_keys=std::make_unique<orchard::WalletKeys>(std::move(owner.keys));
        }
        tx.Commit();
    }
    if(!recovery_keys)return result; // Status reads and existing/Ready jobs never requeue.
    if(eligible){
        Check(head.sequence&&!head.digest.IsNull());
        for(const auto& account:inventory.accounts){
            const auto receipt=account.state.account.Delivery();
            Check(receipt.sequence==head.sequence&&receipt.digest==head.digest);
        }
        // Invokes the immutable source only outside wallet/SQLite ownership.
        Check(requested->state.account.Scan().Checkpoint()==points(head).checkpoint);
    }
    const auto& details=entry.shield_request?*entry.shield_request:*entry.spend_request;
    auto signing=orchard::SigningContext::Create(p.domain,0,entry.inputs,details.outputs,details.fee_una);
    auto plan=orchard::WalletBundlePlan::Restore(*recovery_keys,*entry.recovery);
    recovery_keys.reset();
    const auto intent=plan.Intent(signing);
    Check(intent.Message()==entry.message&&intent.Nullifiers()==entry.nullifiers&&intent.Inputs().size()==entry.inputs.size());
    for(size_t i=0;i<entry.inputs.size();++i){
        const auto& a=intent.Inputs()[i];const auto& b=entry.inputs[i];
        Check(a.txid_wire==b.txid_wire&&a.output_index==b.output_index&&a.sequence==b.sequence&&
            a.amount_una==b.amount_una&&a.script_pub_key==b.script_pub_key);
    }
    std::unique_ptr<OrchardProofJobs::Submission> submission;
    {
        auto lease=w.AcquireDatabaseLease();
        Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        auto fresh=Owner::CaptureCatalog(w,session,p);captured->Recheck(*fresh);
        const OrchardProofJobs::Binding binding{identity,p.domain.branch_id,session,lease->InstanceToken()};
        auto proof=jobs.CaptureOwned(id,binding,queue);
        result.state=proof.state;result.proof=std::move(proof.proof);
        if(!result.state){
            submission=jobs.PrepareOwned(id,binding,std::move(plan),std::move(signing));
            submission->Bind(queue);
        }
        tx.Commit();
    }
    // A failed read/COMMIT drops only the unpublished task; original durable
    // reservations and all existing jobs remain untouched. Publish is noexcept.
    if(submission&&submission->Publish())result.state=OrchardProofJobs::State::Queued;
    return result;
}
std::unique_ptr<OrchardCatalogFinalizationPlan> OrchardAccountDelivery::PrepareFinalizationWithRestorePoints(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,const orchard::Hash& id,
        std::optional<uint64_t> expected,OrchardProofJobs* jobs,bool shield,
        std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    Check(bool(points)&&id!=orchard::Hash{}&&p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
    Check(!shield||(jobs&&!inputs.empty()&&!payments.empty()));
    std::unique_ptr<Owner::CapturedCatalog> captured;
    {
        auto lease=w.AcquireDatabaseLease();
        Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        captured=Owner::CaptureCatalog(w,session,p);tx.Commit();
    }
    // The selected service calls preparation before acquiring its source lock.
    // No caller wallet lease/transaction is accepted, and no source callback is
    // retained in the returned plan or invoked by its writer.
    const auto event=view.Event(1);const auto& context=event->context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    auto inventory=captured->Restore(points);
    const auto requested=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),
        [&](const auto& account){return account.number==p.account;});
    Check(requested!=inventory.accounts.end()&&(!expected||requested->state.revision==*expected));
    const auto index=static_cast<size_t>(requested-inventory.accounts.begin());
    const auto& identity=captured->ownership.accounts[index].identity;
    const auto& queue=requested->state.account.Operations();const auto operation=queue.Entries().find(id);
    Check(operation!=queue.Entries().end());
    if(shield)Check(operation->second.request_commitment==ShieldRequestCommitment(identity,p,id,inputs,payments,outputs,fee));
    else Check(operation->second.inputs.empty());
    auto data=std::make_unique<OrchardCatalogFinalizationPlan::Data>(OrchardCatalogFinalizationPlan::Data{
        std::move(captured),p,session,id,index,std::move(requested->state),jobs,shield,
        {inputs.begin(),inputs.end()},{outputs.begin(),outputs.end()},fee,{},{},{}});
    const auto& operation_entry=data->current.account.Operations().Entries().at(id);
    if(jobs){
        auto proof=jobs->CaptureOwned(id,{identity,p.domain.branch_id,session,data->captured->instance},data->current.account.Operations());
        if(proof.state)Check(proof.state==OrchardProofJobs::State::Succeeded&&proof.proof);
        else Check(operation_entry.phase==OrchardOperationQueue::Phase::Ready);
        data->job_state=proof.state;data->proof=std::move(proof.proof);data->job_token=std::move(proof.token);
    }
    if(operation_entry.phase!=OrchardOperationQueue::Phase::Ready){
        Check(data->current.revision<static_cast<uint64_t>(INT64_MAX)&&
            data->captured->ownership.retained_rows<OrchardOwnershipInventory::kMaxRows);
        auto& retained=data->captured->bytes[index]->retained;retained.reserve(retained.size()+1);
    }
    return std::unique_ptr<OrchardCatalogFinalizationPlan>(new OrchardCatalogFinalizationPlan(std::move(data)));
}
OrchardAccountDelivery::FinalizedProof OrchardAccountDelivery::CommitCatalogFinalization(
        WalletManager& w,uint64_t session,std::unique_ptr<OrchardCatalogFinalizationPlan> plan,
        const consensus::VerifiedOrchardAuthorizations& authorization){
    Check(plan&&plan->data_&&plan->data_->session==session);
    auto data=std::move(plan->data_); // Every outcome consumes this process-local preparation.
    const auto& p=data->profile;const auto& id=data->operation;
    const auto& current=data->current;const auto& entry=current.account.Operations().Entries().at(id);
    const bool already_ready=entry.phase==OrchardOperationQueue::Phase::Ready;
    if(data->shield)CheckShieldSnapshot(authorization.Transparent().Snapshot(),data->inputs);
    else Check(authorization.Transaction().Inputs().empty());
    if(data->proof){
        const auto& envelope=authorization.Transaction();
        const auto exact=data->shield?
            orchard::TransactionEnvelope::Create(0,envelope.Inputs(),data->outputs,data->fee,data->proof->Bytes()):
            orchard::TransactionEnvelope::Create(envelope.LockTime(),{},envelope.Outputs(),envelope.ExplicitFee(),data->proof->Bytes());
        Check(exact.CanonicalBytes()==authorization.Orchard().CanonicalBytes());
    }
    // State preparation and encoding use only restored immutable data and the
    // already-verified authorization. No scan restore or source lookup occurs.
    auto next=[&]{
        if(!data->shield)return current.account.SetReady(id,authorization);
        Check(!already_ready||entry.shield_ready_time.has_value());
        const auto now=std::time(nullptr);Check(already_ready||now>0);
        return current.account.SetShieldReady(id,authorization,already_ready?*entry.shield_ready_time:static_cast<uint64_t>(now));
    }();
    auto encoded=next.Encode();
    FinalizedProof result{{current.revision+(already_ready?0:1),std::move(next)},false};
    static_assert(std::is_nothrow_move_constructible_v<FinalizedProof>);
    const auto& identity=data->captured->ownership.accounts[data->index].identity;
    {
        auto lease=w.AcquireDatabaseLease();
        Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        {auto fresh=Owner::CaptureCatalog(w,session,p);data->captured->Recheck(*fresh);}
        if(data->jobs){
            auto fresh=data->jobs->CaptureOwned(id,{identity,p.domain.branch_id,session,lease->InstanceToken()},current.account.Operations());
            Check(fresh.state==data->job_state&&fresh.token==data->job_token&&bool(fresh.proof)==bool(data->proof));
            if(fresh.proof)Check(fresh.proof->Bytes()==data->proof->Bytes());
        }
        {
            Owner owner(w,session,p,Owner::ExistingRead{});
            Check(Owner::CapturedCatalog::SameIdentity(owner.identity,identity));
            // The exact shield history and Ready snapshot share this FULL
            // transaction; history checks do not bypass the snapshot SQL guards.
            if(data->shield)lease->StageShieldHistoryInTransaction(*owner.seed,
                Owner::ShieldHistory(result.state.account.Operations().Entries().at(id)),already_ready);
            if(!already_ready){
                const auto revision=owner.store.StageReplaceRetaining(current.revision,encoded);
                Check(revision==result.state.revision);
                auto& bytes=*data->captured->bytes[data->index];
                bytes.retained.push_back(std::move(bytes.current));++data->captured->ownership.retained_rows;
                bytes.current={revision,std::move(encoded)};
            }
        }
        // Authenticate every expected account/archive again after the writes.
        // A late SQL refusal or altered other owner rolls back history and Ready.
        {auto fresh=Owner::CaptureCatalog(w,session,p);data->captured->Recheck(*fresh);}
        tx.Commit();
    }
    if(data->jobs)result.retired_job=data->jobs->RetireCaptured(id,data->job_token);
    return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::ReadyCatalogSpendForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,const consensus::VerifiedOrchardAuthorizations& authorization){
    Check(authorization.Transaction().Inputs().empty());
    auto plan=PrepareFinalizationWithRestorePoints(w,session,p,view,id,expected,nullptr,false,{},{},{},0,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
    return CommitCatalogFinalization(w,session,std::move(plan),authorization).state;
}
OrchardAccountDelivery::FinalizedProof OrchardAccountDelivery::FinalizeCatalogProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,const consensus::VerifiedOrchardAuthorizations& authorization,OrchardProofJobs& jobs){
    Check(authorization.Transaction().Inputs().empty());
    auto plan=PrepareFinalizationWithRestorePoints(w,session,p,view,id,{},&jobs,false,{},{},{},0,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
    return CommitCatalogFinalization(w,session,std::move(plan),authorization);
}
std::unique_ptr<orchard::TransactionEnvelope> OrchardAccountDelivery::SignShieldProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,const orchard::Hash& id,
        std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,
        const consensus::OrchardCoinSnapshot& snapshot,OrchardProofJobs& jobs){
    return SignShieldWithRestorePoints(w,session,p,view,id,inputs,payments,outputs,fee,snapshot,jobs,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
std::unique_ptr<orchard::TransactionEnvelope> OrchardAccountDelivery::SignShieldWithRestorePoints(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,const orchard::Hash& id,
        std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,
        const consensus::OrchardCoinSnapshot& snapshot,OrchardProofJobs& jobs,
        const std::function<RestorePoint(RuntimeOutboxCursor)>& points){
    CheckShieldSnapshot(snapshot,inputs);
    Check(bool(points)&&id!=orchard::Hash{}&&p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
    std::unique_ptr<Owner::CapturedCatalog> catalog;
    {
        auto lease=w.AcquireDatabaseLease();
        Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
        RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
        catalog=Owner::CaptureCatalog(w,session,p);tx.Commit();
    }
    // Full restoration and all replay callbacks precede the signing key owner.
    // The service has already released selected ownership after coin capture.
    const auto event=view.Event(1);const auto& context=event->context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    auto inventory=catalog->Restore(points);
    const auto account=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),
        [&](const auto& e){return e.number==p.account;});Check(account!=inventory.accounts.end());
    const auto index=static_cast<size_t>(account-inventory.accounts.begin());
    const auto& identity=catalog->ownership.accounts[index].identity;
    const auto& queue=account->state.account.Operations();const auto found=queue.Entries().find(id);
    Check(found!=queue.Entries().end()&&found->second.phase==OrchardOperationQueue::Phase::Reserved&&
          !account->state.account.Observations().contains(id)&&
          found->second.request_commitment==ShieldRequestCommitment(identity,p,id,inputs,payments,outputs,fee));
    auto lease=w.AcquireDatabaseLease();
    Check(lease->Session()==session&&lease->Database()&&w.database_leases_==1);
    RequireShieldFullSync(lease->Database());Transaction tx(lease->Database());
    {auto fresh=Owner::CaptureCatalog(w,session,p);catalog->Recheck(*fresh);}
    std::unique_ptr<orchard::TransactionEnvelope> result;
    {
        Owner owner(w,session,p,Owner::ExistingRead{});
        Check(Owner::CapturedCatalog::SameIdentity(owner.identity,identity));
        auto captured=jobs.CaptureOwned(id,{owner.identity,p.domain.branch_id,session,lease->InstanceToken()},queue);
        Check(captured.state==OrchardProofJobs::State::Succeeded&&captured.proof);
        const auto& envelope=snapshot.Transaction();auto signed_inputs=envelope.Inputs();
        for(const auto& input:signed_inputs)Check(input.witness.empty());
        const std::vector<orchard::TransparentOutput> requested_outputs(outputs.begin(),outputs.end());
        const auto exact=orchard::TransactionEnvelope::Create(0,signed_inputs,requested_outputs,fee,captured.proof->Bytes());
        Check(exact.CanonicalBytes()==envelope.CanonicalBytes()&&snapshot.SigningDigest(p.domain)==found->second.message);
        for(size_t i=0;i<inputs.size();++i){
            auto key=lease->ResolveSigningKeyInTransaction(util::hex(inputs[i].script_pub_key),*owner.seed);
            Check(key&&key->script==inputs[i].script_pub_key);
            signed_inputs[i].witness=SignShieldInput(*key,consensus::OrchardTransparentSigningDigest(snapshot,p.domain,i));
        }
        result=std::make_unique<orchard::TransactionEnvelope>(
            orchard::TransactionEnvelope::Create(0,signed_inputs,requested_outputs,fee,captured.proof->Bytes()));
    }
    // Release the signing seed before the second complete catalog capture.
    // Failure returns no signatures and never retires the proof or reservation.
    {auto fresh=Owner::CaptureCatalog(w,session,p);catalog->Recheck(*fresh);}
    tx.Commit();return result; // Service retains bytes privately until Ready commits.
}
std::unique_ptr<OrchardCatalogFinalizationPlan> OrchardAccountDelivery::PrepareShieldFinalizationForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,const orchard::Hash& id,
        std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs){
    return PrepareFinalizationWithRestorePoints(w,session,p,view,id,{},&jobs,true,inputs,payments,outputs,fee,
        [&view](RuntimeOutboxCursor cursor){return view.Point(cursor);});
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::ApplyForReplay(WalletManager& w,uint64_t session,const Profile& p,
        uint64_t expected,const RuntimeAccountReplay& view,uint64_t sequence){
    const auto event_handle=view.Event(sequence);const auto& event=*event_handle;
    Owner owner(w,session,p);Transaction tx(owner.lease->Database());
    auto current=owner.RestoreReplay(p,view);Check(current.revision==expected);
    current=owner.Reconcile(std::move(current),p,view);
    auto result=[&]{
        if(!event.IsOrchardProfile())return owner.Replace(current.revision,current.account.ApplyHistoricalDelivery(event));
        if(event.direction==RuntimeBlockDirection::Connect)
            return owner.Replace(current.revision,current.account.AdvanceDelivery(event,*view.Block(sequence),*view.State(sequence),*view.Authorizations(sequence))
                .WithParentSnapshotRevision(current.revision));
        return owner.Undo(p,current,event,*view.Block(sequence),view.Point(event.cursor));
    }();
    result=owner.Reconcile(std::move(result),p,view);tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::ReconcileForReplay(WalletManager& w,uint64_t session,const Profile& p,
        uint64_t expected,const RuntimeAccountReplay& view){
    Owner owner(w,session,p);Transaction tx(owner.lease->Database());
    auto current=owner.RestoreReplay(p,view);Check(current.revision==expected);
    auto result=owner.Reconcile(std::move(current),p,view);tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Connect(WalletManager& w,uint64_t s,const Profile& p,uint64_t expected,
        const RestorePoint& point,const RuntimeOutboxEvent& event,const OrchardBlockCandidate& block,
        const consensus::PreparedOrchardState& state,std::span<const consensus::VerifiedOrchardAuthorizations> auths){
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());auto current=owner.Restore(p,point);Check(current.revision==expected);
    auto result=owner.Replace(expected,current.account.AdvanceDelivery(event,block,state,auths).WithParentSnapshotRevision(expected));tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Disconnect(WalletManager& w,uint64_t s,const Profile& p,uint64_t expected,
        const RestorePoint& point,const RuntimeOutboxEvent& event,const OrchardBlockCandidate& block,const RestorePoint& parent){
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());auto current=owner.Restore(p,point);Check(current.revision==expected);
    auto result=owner.Undo(p,current,event,block,parent);tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Historical(WalletManager& w,uint64_t s,const Profile& p,uint64_t expected,
        const RestorePoint& point,const RuntimeOutboxEvent& event){
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());auto current=owner.Restore(p,point);Check(current.revision==expected);
    auto result=owner.Replace(expected,current.account.ApplyHistoricalDelivery(event));tx.Commit();return result;
}
} // namespace dinero::wallet
