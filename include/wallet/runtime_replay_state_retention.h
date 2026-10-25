#pragma once
#include "consensus/orchard_state_transition.h"
#include "wallet/runtime_replay_spool_codec.h"
#include <openssl/sha.h>
#include <list>
#include <limits>

namespace dinero { class RuntimeAccountReplay; }
namespace dinero::wallet::detail {
// Process-local retention of an ALREADY prepared state. This is not a decoder
// for certificates supplied by callers or read from an existing database.
// Only Capture(PreparedOrchardState) can mint a token. Its original content
// digest lives with that token, independently of the spool's storage MAC.
// Thus authenticating an arbitrary spool record cannot create state authority.
class RuntimeReplayStateRetention {
    struct Identity {};
public:
    class Token {
    public:
        Token(const Token&)=default;
        Token(Token&&)=default;
        Token& operator=(const Token&)=default;
        Token& operator=(Token&&)=default;
    private:
        friend class RuntimeReplayStateRetention;
        Token(std::shared_ptr<const Identity> owner,uint64_t id,std::array<uint8_t,32> digest)
            :owner_(std::move(owner)),id_(id),digest_(digest){}
        std::shared_ptr<const Identity> owner_;
        uint64_t id_;
        std::array<uint8_t,32> digest_;
    };
    using Handle=std::shared_ptr<const consensus::PreparedOrchardState>;
    RuntimeReplayStateRetention()=default;
    RuntimeReplayStateRetention(const RuntimeReplayStateRetention&)=delete;
    RuntimeReplayStateRetention& operator=(const RuntimeReplayStateRetention&)=delete;

    Token Capture(const consensus::PreparedOrchardState& state) {
        // The only entry to the archive accepts the sealed type, never raw
        // state fields or a caller-selected digest/record identifier.
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
    friend class dinero::RuntimeAccountReplay;
    // Only the replay's content-addressed index may retain these bytes. Its
    // in-memory root binds the originally minted digest independently of this
    // state's scratch spool MAC. Never expose a raw token import API.
    RuntimeReplaySpool::Bytes IndexToken(const Token& token) const {
        Check(token.owner_==identity_&&token.id_>0&&token.id_<next_);
        replay_spool_codec::Writer w;w.Number(token.id_,8);w.Raw(token.digest_);return std::move(w.bytes);
    }
    Token IndexToken(std::span<const uint8_t> bytes) const {
        replay_spool_codec::Reader r{bytes};const auto id=r.Number(8);std::array<uint8_t,32> digest{};
        const auto raw=r.Raw(32);std::copy(raw.begin(),raw.end(),digest.begin());
        Check(r.bytes.empty()&&id>0&&id<next_);return Token(identity_,id,digest);
    }
    friend struct RuntimeReplayStateRetentionTestAccess;
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
    static Bytes Key(uint64_t id){replay_spool_codec::Writer w;w.Number('s',1);w.Number(id,8);return std::move(w.bytes);}
    static Bytes Encode(const consensus::PreparedOrchardState& state){
        replay_spool_codec::Writer w;w.Number(1,1);w.Number(state.Parent().has_value(),1);
        if(state.Parent())w.State(*state.Parent());w.State(state.Next());
        w.Number(state.Nullifiers().size(),4);for(const auto& n:state.Nullifiers())w.Hash(n);
        w.Number(state.Flows().size(),4);for(const auto& f:state.Flows()){
            w.Number(f.transparent_inputs,8);w.Number(f.transparent_outputs,8);w.Number(f.fee,8);
        }
        w.Number(state.Fees(),8);return std::move(w.bytes);
    }
    static Handle Decode(std::span<const uint8_t> bytes){
        replay_spool_codec::Reader r{bytes};Check(r.Number(1)==1);
        const auto has_parent=r.Number(1);Check(has_parent<=1);
        std::optional<storage::OrchardStoredState> parent;if(has_parent)parent=r.State();
        auto next=r.State();const auto count=r.Number(4);Check(count<=r.bytes.size()/32);
        std::vector<uint256> nullifiers;nullifiers.reserve(count);
        for(uint64_t i=0;i<count;++i)nullifiers.push_back(r.Hash());
        const auto flows_count=r.Number(4);Check(flows_count<=r.bytes.size()/24);
        std::vector<consensus::OrchardValueFlow> flows;flows.reserve(flows_count);
        for(uint64_t i=0;i<flows_count;++i)flows.push_back({r.Number(8),r.Number(8),r.Number(8)});
        const auto fees=r.Number(8);Check(r.bytes.empty());
        return Handle(new consensus::PreparedOrchardState(std::move(parent),std::move(next),
            std::move(nullifiers),std::move(flows),fees));
    }
};
} // namespace dinero::wallet::detail
