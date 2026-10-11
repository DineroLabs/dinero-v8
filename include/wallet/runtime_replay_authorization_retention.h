#pragma once
#include "consensus/orchard_authorization.h"
#include "wallet/runtime_replay_spool_codec.h"
#include <openssl/sha.h>
#include <list>
#include <limits>

namespace dinero::wallet::detail {
// Process-local retention of ALREADY verified transaction authorizations. This is not a decoder
// for certificates supplied by callers or read from an existing database.
// Only Capture(vector<VerifiedOrchardAuthorizations>) can mint a token. Its original content
// digest lives with that token, independently of the spool's storage MAC.
// Thus authenticating an arbitrary spool record cannot create authorization authority.
class RuntimeReplayAuthorizationRetention {
    struct Identity {};
public:
    class Token {
    public:
        Token(const Token&)=default;
        Token(Token&&)=default;
        Token& operator=(const Token&)=default;
        Token& operator=(Token&&)=default;
    private:
        friend class RuntimeReplayAuthorizationRetention;
        Token(std::shared_ptr<const Identity> owner,uint64_t id,std::array<uint8_t,32> digest)
            :owner_(std::move(owner)),id_(id),digest_(digest){}
        std::shared_ptr<const Identity> owner_;
        uint64_t id_;
        std::array<uint8_t,32> digest_;
    };
    using Handle=std::shared_ptr<const std::vector<consensus::VerifiedOrchardAuthorizations>>;
    RuntimeReplayAuthorizationRetention()=default;
    RuntimeReplayAuthorizationRetention(const RuntimeReplayAuthorizationRetention&)=delete;
    RuntimeReplayAuthorizationRetention& operator=(const RuntimeReplayAuthorizationRetention&)=delete;

    Token Capture(const std::vector<consensus::VerifiedOrchardAuthorizations>& state) {
        // The only entry to the archive accepts the sealed type, never raw
        // transaction fields or a caller-selected digest/record identifier.
        const auto encoded=Encode(state);const auto digest=Digest(encoded);
        std::lock_guard guard(mutex_);
        Check(!frozen_&&next_!=std::numeric_limits<uint64_t>::max());
        const auto id=next_;
        {RuntimeReplaySpool::Batch batch(spool_);spool_.Insert(Key(id),encoded);batch.Commit();}
        ++next_;
        return Token(identity_,id,digest);
    }
    Handle Load(const Token& token) const {
        std::lock_guard guard(mutex_);
        Check(token.owner_==identity_&&token.id_>0&&token.id_<next_);
        for(auto it=cache_.begin();it!=cache_.end();++it) {
            if(it->id!=token.id_)continue;
            Check(it->digest==token.digest_);
            const auto result=it->value;cache_.splice(cache_.begin(),cache_,it);return result;
        }
        const auto bytes=spool_.Get(Key(token.id_));Check(bytes.has_value());
        // Verify the digest originally minted from the sealed value BEFORE
        // decoding or invoking its private constructor. The spool MAC alone
        // does not authorize construction. No proof/chain callback occurs.
        Check(Digest(*bytes)==token.digest_);
        auto value=Decode(*bytes);
        cache_.push_front({token.id_,token.digest_,value,bytes->size()});cache_bytes_+=bytes->size();
        while(cache_.size()>CacheEntries||cache_bytes_>CacheBytes) {
            cache_bytes_-=cache_.back().charge;cache_.pop_back();
        }
        return value;
    }
    void Freeze(){std::lock_guard guard(mutex_);Check(!frozen_);spool_.Freeze();frozen_=true;}
private:
    friend struct RuntimeReplayAuthorizationRetentionTestAccess;
    using Bytes=RuntimeReplaySpool::Bytes;
    static constexpr size_t CacheEntries=4,CacheBytes=RuntimeReplaySpool::MaximumRecordBytes;
    struct Cached {uint64_t id;std::array<uint8_t,32> digest;Handle value;size_t charge;};
    const std::shared_ptr<const Identity> identity_=std::make_shared<const Identity>();
    RuntimeReplaySpool spool_;
    uint64_t next_=1;bool frozen_=false;
    mutable std::mutex mutex_;
    mutable std::list<Cached> cache_;
    mutable size_t cache_bytes_=0;
    static void Check(bool value){if(!value)throw consensus::OrchardStateLookupError(Status::Corruption);}
    static std::array<uint8_t,32> Digest(std::span<const uint8_t> bytes){
        std::array<uint8_t,32> result{};Check(SHA256(bytes.data(),bytes.size(),result.data())!=nullptr);return result;
    }
    static Bytes Key(uint64_t id){replay_spool_codec::Writer w;w.Number('a',1);w.Number(id,8);return std::move(w.bytes);}
    static Bytes Encode(const std::vector<consensus::VerifiedOrchardAuthorizations>& values){
        replay_spool_codec::Writer w;w.Number(1,1);w.Number(values.size(),4);
        for(const auto& value:values){
            const auto& transparent=value.Transparent();const auto& snapshot=transparent.Snapshot();
            const auto& tx=value.Transaction();const auto& auth=value.Orchard();
            Check(tx.CanonicalBytes()==auth.CanonicalBytes()&&tx.Txid()==auth.Txid()&&tx.Wtxid()==auth.Wtxid());
            w.Blob(tx.CanonicalBytes());w.Number(snapshot.ViewHeight(),4);w.Number(transparent.CandidateHeight(),4);
            w.Raw(transparent.OrchardIntent());w.Raw(auth.Orchard().SigningDigest());
            w.Number(snapshot.Coins().size(),4);
            for(const auto& coin:snapshot.Coins()){
                w.Number(coin.value.GetUna(),8);w.Blob(coin.scriptPubKey);w.Number(coin.height,4);
                w.Number(coin.isCoinbase,1);w.Number(coin.is_confidential,1);w.Blob(coin.commitment);
            }
        }
        return std::move(w.bytes);
    }
    static orchard::Hash ReadHash(replay_spool_codec::Reader& r){
        const auto bytes=r.Raw(32);orchard::Hash h{};std::copy(bytes.begin(),bytes.end(),h.begin());return h;
    }
    static Handle Decode(std::span<const uint8_t> bytes){
        // Called exclusively AFTER the token's original-content digest check.
        // Parsing the original Rust bundle does not repeat proof verification.
        replay_spool_codec::Reader r{bytes};Check(r.Number(1)==1);const auto count=r.Number(4);
        Check(count<=r.bytes.size()/80);std::vector<consensus::VerifiedOrchardAuthorizations> values;values.reserve(count);
        for(uint64_t i=0;i<count;++i){
            const auto wire=r.Blob(orchard::kMaxTransactionBytes);auto tx=orchard::TransactionEnvelope::DecodeExact(wire);
            const auto view=uint32_t(r.Number(4)),height=uint32_t(r.Number(4));
            const auto intent=ReadHash(r),digest=ReadHash(r);const auto coins_count=r.Number(4);
            Check(coins_count==tx.Inputs().size()&&coins_count<=r.bytes.size()/22);
            std::vector<consensus::UTXOEntry> coins;coins.reserve(coins_count);
            for(uint64_t j=0;j<coins_count;++j){
                const auto amount=r.Number(8);auto script=r.Blob();const auto coin_height=uint32_t(r.Number(4));
                const auto coinbase=r.Number(1),confidential=r.Number(1);Check(coinbase<=1&&confidential<=1);
                auto commitment=r.Blob();coins.emplace_back(AmountUna::Una(amount),script,coin_height,bool(coinbase),bool(confidential),commitment);
            }
            // Each private constructor is reachable only through Load's owner,
            // slot and original digest checks. A generic valid storage MAC is
            // insufficient, and no public raw-authorization decoder exists.
            orchard::VerifiedAuthorization inner(tx.bundle_.handle_,tx.bundle_.facts_,digest);
            orchard::VerifiedEnvelopeAuthorization envelope(std::move(inner),tx.Txid(),tx.Wtxid(),wire);
            consensus::OrchardCoinSnapshot snapshot(std::move(tx),std::move(coins),view);
            consensus::VerifiedOrchardTransparentInputs transparent(std::move(snapshot),intent,height);
            values.push_back(consensus::VerifiedOrchardAuthorizations(std::move(transparent),std::move(envelope)));
        }
        Check(r.bytes.empty());return std::make_shared<const std::vector<consensus::VerifiedOrchardAuthorizations>>(std::move(values));
    }
};
} // namespace dinero::wallet::detail
