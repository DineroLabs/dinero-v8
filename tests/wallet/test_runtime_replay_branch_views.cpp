#include "../consensus/orchard_block_test_fixture.h"
#include "consensus/undo.h"
#include "primitives/block.h"
#include "wallet/runtime_account_replay.h"
#include "wallet/runtime_replay_spool_codec.h"
#include <set>
#include <type_traits>
#include "wallet/runtime_replay_state_retention.h"
#include "wallet/runtime_replay_authorization_retention.h"
#include "orchard_wallet.h"

namespace dinero {
struct RuntimeAccountReplayTestAccess {
    static auto Capture(const std::vector<RuntimeOutboxEvent>& events) {
        Require(!events.empty());
        return RuntimeAccountReplay::Capture([&](RuntimeOutboxCursor after,size_t maximum) {
            Require(after.sequence<=events.size());
            Require(after.sequence?events[after.sequence-1].cursor==after:after.digest.IsNull());
            RuntimeOutboxPage page;page.head=events.back().cursor;page.next=after;
            if(after.sequence) {
                const auto& event=events[after.sequence-1];
                const bool connect=event.direction==RuntimeBlockDirection::Connect;
                page.after_tip=std::pair{connect?event.context.block_hash:event.context.parent_hash,
                                        event.context.height-(connect?0u:1u)};
            }
            // Force more than one source page, including the final EOF read.
            const size_t end=std::min(events.size(),size_t(after.sequence)+std::min(maximum,size_t(2)));
            for(size_t i=after.sequence;i<end;++i)page.events.push_back(events[i]);
            if(!page.events.empty())page.next=page.events.back().cursor;
            return page;
        });
    }
};
}
namespace dinero::wallet::detail {
struct RuntimeReplayAuthorizationRetentionTestAccess {
    static RuntimeReplaySpool& Spool(RuntimeReplayAuthorizationRetention& value){return value.spool_;}
};
struct RuntimeReplayStateRetentionTestAccess {
    static RuntimeReplaySpool& Spool(RuntimeReplayStateRetention& value){return value.spool_;}
};
struct RuntimeReplaySpoolTestAccess {
    static sqlite3* Handle(RuntimeReplaySpool& value){return value.db_.get();}
    static void ChangeValueWithValidStorageMac(RuntimeReplaySpool& value,uint8_t prefix='s') {
        const RuntimeReplaySpool::Bytes key{prefix,1,0,0,0,0,0,0,0};
        auto bytes=value.Get(key);Require(bytes.has_value()&&!bytes->empty());
        if(prefix=='a') {
            // Alter the recorded verified digest, preserving the entire codec
            // shape. A decoder alone accepts this; only original-content
            // binding prevents it from minting a changed authorization.
            replay_spool_codec::Reader reader{*bytes};
            Require(reader.Number(1)==1&&reader.Number(4)>0);
            (void)reader.Blob(orchard::kMaxTransactionBytes);
            (void)reader.Number(4);(void)reader.Number(4);(void)reader.Raw(32);
            const size_t digest_offset=bytes->size()-reader.bytes.size();
            Require(reader.bytes.size()>=32);(*bytes)[digest_offset]^=1;
        } else bytes->back()^=1;
        std::array<uint8_t,32> tag;
        {std::lock_guard lock(value.mutex_);tag=value.Tag(key,*bytes);}
        sqlite3_stmt* statement=nullptr;
        Require(sqlite3_prepare_v2(value.db_.get(),"UPDATE records SET v=?1,tag=?2 WHERE k=?3",-1,&statement,nullptr)==SQLITE_OK);
        Require(sqlite3_bind_blob(statement,1,bytes->data(),int(bytes->size()),SQLITE_TRANSIENT)==SQLITE_OK);
        Require(sqlite3_bind_blob(statement,2,tag.data(),int(tag.size()),SQLITE_TRANSIENT)==SQLITE_OK);
        Require(sqlite3_bind_blob(statement,3,key.data(),int(key.size()),SQLITE_TRANSIENT)==SQLITE_OK);
        const int result=sqlite3_step(statement);sqlite3_finalize(statement);Require(result==SQLITE_DONE);
        Require(value.Get(key)==bytes); // Generic storage MAC accepts it.
    }
};
}
namespace {
// Local component source, not durable outbox or historical consensus evidence.
// Real signatures/proofs are verified again by RuntimeAccountReplay::Build.
struct Branch {
    std::optional<storage::OrchardStoredState> checkpoint;
    std::set<uint256> nullifiers,anchors;
    uint256 hash;uint32_t height;
};
struct Trace {
    orchard::SigningDomain domain;
    std::vector<RuntimeOutboxEvent> events;
    explicit Trace(const std::string& base):domain(Fixture(base).domain){}
    void Append(RuntimeOutboxEvent event) {
        event.previous_digest=events.empty()?uint256{}:events.back().cursor.digest;
        Require(events.size()<200);event.cursor={events.size()+1,H(uint8_t(events.size()+1))};
        events.push_back(std::move(event));
    }
    Branch Connect(const Branch& parent,std::span<const VerifiedOrchardAuthorizations> auths,uint32_t nonce) {
        OrchardBlockContext context{parent.height+1,{},parent.hash,20001,domain};
        const auto block=Candidate(context,auths,{},nonce);context.block_hash=block.Header().GetHash();
        OrchardStateLookups lookups{
            [&](const uint256& hash)->StatusOr<bool>{return parent.anchors.contains(hash);},
            [&](const uint256& hash)->StatusOr<bool>{return parent.nullifiers.contains(hash);}};
        const auto state=PrepareOrchardStateTransition(context,parent.checkpoint,auths,lookups);
        UndoRecord undo;
        for(const auto& auth:auths) {
            const auto& snapshot=auth.Transparent().Snapshot();
            const auto& inputs=snapshot.Transaction().Inputs();const auto& coins=snapshot.Coins();
            Require(inputs.size()==coins.size());
            for(size_t i=0;i<inputs.size();++i) {
                const auto point=::Point(inputs[i]);const auto& coin=coins[i];
                undo.spent.emplace_back(point.txid.AsUint256(),point.vout,coin.value.GetUna(),
                    coin.scriptPubKey,coin.isCoinbase,coin.height,coin.is_confidential,coin.commitment);
            }
        }
        for(const auto& transaction:block.Transactions()) {
            const size_t count=transaction.IsOrchard()?transaction.Orchard().Outputs().size():transaction.Historical().vout.size();
            for(size_t i=0;i<count;++i)undo.created.emplace_back(transaction.GetTxid().AsUint256(),uint32_t(i));
        }
        RuntimeOutboxEvent event;event.direction=RuntimeBlockDirection::Connect;event.context=context;
        event.body=block.WireBytes();event.orchard_replay=RuntimeOrchardReplay{parent.checkpoint,state.Next(),undo.Serialize(),{}};
        Append(std::move(event));
        Branch next=parent;next.checkpoint=state.Next();next.height=context.height;next.hash=context.block_hash;
        for(const auto& nf:state.Nullifiers())Require(next.nullifiers.insert(nf).second);
        next.anchors.insert(state.Next().anchor);return next;
    }
    void Disconnect(size_t connected) {
        Require(connected<events.size());auto event=events[connected];
        Require(event.direction==RuntimeBlockDirection::Connect);event.direction=RuntimeBlockDirection::Disconnect;
        Append(std::move(event));
    }
};
void BranchMembership(const std::string& base) {
    const std::vector<VerifiedOrchardAuthorizations> shield{Authorized(base,false,20000)};
    const std::vector<VerifiedOrchardAuthorizations> spend{Authorized(base,true,20001)};
    Trace trace(base);const Branch origin{{},{},{},H(240),20000};
    const auto funded=trace.Connect(origin,shield,1);
    const auto first_spend=trace.Connect(funded,spend,2);
    trace.Disconnect(1);
    const auto sibling=trace.Connect(funded,{},3);
    // Authorization seals candidate height, so revalidate at this new parent.
    const std::vector<VerifiedOrchardAuthorizations> sibling_auths{Authorized(base,true,sibling.height)};
    Require(sibling_auths.front().Transaction().CanonicalBytes()==spend.front().Transaction().CanonicalBytes());
    const auto sibling_spend=trace.Connect(sibling,sibling_auths,4);
    Require(first_spend.hash!=sibling.hash&&first_spend.hash!=sibling_spend.hash);
    const auto view=RuntimeAccountReplayTestAccess::Capture(trace.events);
    Require(view->Head()==trace.events.back().cursor);
    const auto fund_state=view->State(1),spent_state=view->State(2);Require(!fund_state->Nullifiers().empty()&&!spent_state->Nullifiers().empty());
    const auto nf=spent_state->Nullifiers().front();
    for(size_t i=0;i<trace.events.size();++i) {
        const auto point=view->Point(trace.events[i].cursor);
        Require(*point.lookups.spent_nullifier(nf)==(i==1||i==4));
        for(const auto& common:fund_state->Nullifiers())Require(*point.lookups.spent_nullifier(common));
    }
    const auto branch_point=view->Point(trace.events[3].cursor);
    LookupReject(Status::Corruption,[&]{(void)branch_point.lookups.selected_block(first_spend.height,first_spend.hash);});
    Require(branch_point.lookups.selected_block(funded.height,funded.hash)->Header().GetHash()==funded.hash);
    Require(view->ForkHeight(trace.events.back().cursor,first_spend.hash,first_spend.height)==funded.height);
    Require(view->IsAncestorOf(trace.events[0].cursor,sibling_spend.hash,sibling_spend.height));
    Require(!view->IsAncestorOf(trace.events[1].cursor,sibling_spend.hash,sibling_spend.height));
    const auto selected=view->SelectedHashes(trace.events.back().cursor);
    Require(*selected(funded.height)==funded.hash&&*selected(sibling.height)==sibling.hash);
    Require(*selected(sibling_spend.height)==sibling_spend.hash);
    std::cout<<"RuntimeReplayBranchViews BranchMembership PASS\n";

    // Rewind past funding, choose an empty activation block, and try the real
    // signed spend against its abandoned funding root. The new selected
    // parent is genuine; the old root must not leak from the retained branch.
    Trace anchor_trace(base);
    (void)anchor_trace.Connect(origin,shield,10);
    anchor_trace.Disconnect(0);
    const auto empty_branch=anchor_trace.Connect(origin,{},11);
    const auto empty_view=RuntimeAccountReplayTestAccess::Capture(anchor_trace.events);
    Require(empty_view->Point(anchor_trace.events.back().cursor).checkpoint==*empty_branch.checkpoint);
    OrchardBlockContext refused_context{empty_branch.height+1,{},empty_branch.hash,20001,anchor_trace.domain};
    const auto refused_block=Candidate(refused_context,spend,{},12);
    refused_context.block_hash=refused_block.Header().GetHash();
    RuntimeOutboxEvent refused;refused.direction=RuntimeBlockDirection::Connect;
    refused.context=refused_context;refused.body=refused_block.WireBytes();
    // A retained old Next is deliberately unusable on this sibling. Anchor
    // admission must reject before Next equality is ever considered.
    refused.orchard_replay=RuntimeOrchardReplay{empty_branch.checkpoint,*first_spend.checkpoint,
        trace.events[1].orchard_replay->coin_undo,{}};
    anchor_trace.Append(std::move(refused));
    StateReject(StateError::Anchor,[&]{(void)RuntimeAccountReplayTestAccess::Capture(anchor_trace.events);});
    Require(empty_view->Head()==anchor_trace.events[2].cursor);
    Require(!*empty_view->Point(anchor_trace.events[2].cursor).lookups.spent_nullifier(nf));
    Require(*view->Point(trace.events[4].cursor).lookups.spent_nullifier(nf));
    std::cout<<"RuntimeReplayBranchViews AbandonedAnchorRefuses PASS\n";

    // A checkpoint altered after genuine preparation must fail revalidation.
    auto corrupt=trace.events;++corrupt.back().orchard_replay->next.pool_balance;
    LookupReject(Status::Corruption,[&]{(void)RuntimeAccountReplayTestAccess::Capture(corrupt);});
    Require(*view->Point(trace.events[3].cursor).lookups.spent_nullifier(nf)==false);
    Require(*view->Point(trace.events.back().cursor).lookups.spent_nullifier(nf));
    std::cout<<"RuntimeReplayBranchViews PreparedStateMismatchRefuses PASS\n";

    RuntimeAccountReplay::EventHandle event;
    RuntimeAccountReplay::BlockHandle block;
    RuntimeAccountReplay::StateHandle state;
    RuntimeAccountReplay::AuthorizationHandle authorizations;
    std::weak_ptr<const RuntimeOutboxEvent> unrelated;
    {
        const auto owned=RuntimeAccountReplayTestAccess::Capture(trace.events);
        event=owned->Event(1);block=owned->Block(1);state=owned->State(1);authorizations=owned->Authorizations(1);
        unrelated=owned->Event(2);Require(!unrelated.expired());
    }
    Require(unrelated.expired());
    Require(event->body==block->WireBytes()&&state->Next()==*funded.checkpoint);
    Require(authorizations->size()==shield.size());
    Require(authorizations->front().Transaction().CanonicalBytes()==shield.front().Transaction().CanonicalBytes());
    std::cout<<"RuntimeReplayBranchViews IndependentHandleLifetimes PASS\n";
}
}
namespace {
void EventEvictionReload(const std::string& base) {
    Trace trace(base);Branch parent{{},{},{},H(239),20000};
    for(uint32_t i=0;i<7;++i)parent=trace.Connect(parent,{},40+i);
    const auto view=RuntimeAccountReplayTestAccess::Capture(trace.events);
    const auto held=view->Event(1);
    Require(view->Event(1)==held);
    std::weak_ptr<const RuntimeOutboxEvent> evicted=view->Event(2);
    Require(!evicted.expired());
    for(uint64_t sequence=3;sequence<=6;++sequence) {
        const auto event=view->Event(sequence);
        Require(wallet::detail::replay_spool_codec::Encode(*event)==
                wallet::detail::replay_spool_codec::Encode(trace.events[sequence-1]));
    }
    Require(evicted.expired());
    const auto reloaded=view->Event(2);
    Require(wallet::detail::replay_spool_codec::Encode(*reloaded)==
            wallet::detail::replay_spool_codec::Encode(trace.events[1]));
    Require(view->Event(2)==reloaded);
    Require(wallet::detail::replay_spool_codec::Encode(*held)==
            wallet::detail::replay_spool_codec::Encode(trace.events[0]));
    std::cout<<"RuntimeReplayBranchViews EventEvictionReload PASS\n";
}
void BodyEvictionReload(const std::string& base) {
    Trace trace(base);Branch parent{{},{},{},H(238),20000};
    for(uint32_t i=0;i<7;++i)parent=trace.Connect(parent,{},60+i);
    RuntimeAccountReplay::BlockHandle held;
    std::weak_ptr<const OrchardBlockCandidate> evicted;
    {
        const auto view=RuntimeAccountReplayTestAccess::Capture(trace.events);
        held=view->Block(1);Require(view->Block(1)==held);
        evicted=view->Block(2);Require(!evicted.expired());
        for(uint64_t sequence=3;sequence<=6;++sequence)
            Require(view->Block(sequence)->WireBytes()==trace.events[sequence-1].body);
        Require(evicted.expired());
        const auto reloaded=view->Block(2);
        Require(reloaded->WireBytes()==trace.events[1].body&&view->Block(2)==reloaded);
        const auto point=view->Point(trace.events.back().cursor);
        Require(point.lookups.selected_block(trace.events[0].context.height,
            trace.events[0].context.block_hash)->WireBytes()==held->WireBytes());
    }
    Require(held->WireBytes()==trace.events[0].body);
    std::cout<<"RuntimeReplayBranchViews BodyEvictionReload PASS\n";
}
void HistoricalBodyEvictionReload(const std::string& base) {
    // Real historical block encoding and an activation-crossing event trace;
    // no historical PoW/script/unspentness certification is claimed.
    std::vector<dinero::Block> history;uint256 previous=H(237);
    for(uint32_t i=0;i<7;++i) {
        dinero::Block block{};Transaction coinbase;coinbase.vin.resize(1);
        coinbase.vin[0].prevout=TxOutPoint(TxId(uint256{}),UINT32_MAX);
        coinbase.vin[0].scriptSig=CoinbaseHeightScript(19994+i);
        coinbase.vout.emplace_back(AmountUna::Zero(),std::vector<uint8_t>{0x51});
        block.vtx.push_back(coinbase);block.header.version=1;
        block.header.prev_block_hash=previous;block.header.timestamp=19994+i;
        block.header.merkle_root=ComputeMerkleRoot(block.vtx);
        previous=block.GetHash();history.push_back(std::move(block));
    }
    Trace trace(base);const Branch origin{{},{},{},history.back().GetHash(),20000};
    (void)trace.Connect(origin,{},80);trace.Disconnect(0);
    auto append=[&](size_t i,RuntimeBlockDirection direction) {
        const auto& block=history[i];RuntimeOutboxEvent event;event.direction=direction;
        event.context={uint32_t(19994+i),block.GetHash(),block.header.prev_block_hash,20001,trace.domain};
        const auto wire=block.Serialize();event.body.assign(wire.begin(),wire.end());trace.Append(std::move(event));
    };
    for(size_t i=history.size();i>0;--i)append(i-1,RuntimeBlockDirection::Disconnect);
    for(size_t i=0;i<history.size();++i)append(i,RuntimeBlockDirection::Connect);
    std::shared_ptr<const dinero::Block> held;
    {
        const auto view=RuntimeAccountReplayTestAccess::Capture(trace.events);
        const auto point=view->Point(view->Head());
        Require(point.checkpoint.height==20000&&point.checkpoint.block_hash==history.back().GetHash());
        auto read=[&](size_t i){return point.lookups.selected_historical_block(uint32_t(19994+i),history[i].GetHash());};
        held=read(0);Require(read(0)==held);
        std::weak_ptr<const dinero::Block> evicted=read(1);Require(!evicted.expired());
        for(size_t i=2;i<6;++i)Require(read(i)->Serialize()==history[i].Serialize());
        Require(evicted.expired());
        const auto reloaded=read(1);Require(reloaded->Serialize()==history[1].Serialize()&&read(1)==reloaded);
        Require(view->Block(1)->WireBytes()==trace.events.front().body);
        // The same recorded historical header cannot name a changed body on
        // reconnect. Preserve the existing exact-body consistency refusal.
        auto corrupt=trace.events;auto changed=history.back();
        changed.vtx.front().vout.front().value=AmountUna::Una(1);
        const auto wire=changed.Serialize();corrupt.back().body.assign(wire.begin(),wire.end());
        LookupReject(Status::Corruption,[&]{(void)RuntimeAccountReplayTestAccess::Capture(corrupt);});
    }
    Require(held->Serialize()==history.front().Serialize());
    std::cout<<"RuntimeReplayBranchViews HistoricalBodyEvictionReload PASS\n";
}
void StateEvictionReload(const std::string& base) {
    Trace trace(base);Branch parent{{},{},{},H(240),20000};
    const std::vector<VerifiedOrchardAuthorizations> shield{Authorized(base,false,20000)};
    parent=trace.Connect(parent,shield,71);
    for(uint32_t i=0;i<7;++i)parent=trace.Connect(parent,{},72+i);
    RuntimeAccountReplay::StateHandle held;
    {
        const auto view=RuntimeAccountReplayTestAccess::Capture(trace.events);
        held=view->State(1);Require(!held->Nullifiers().empty()&&view->State(1)==held);
        std::weak_ptr<const PreparedOrchardState> evicted=view->State(2);Require(!evicted.expired());
        for(uint64_t i=3;i<8;++i)Require(view->State(i)->Next()==trace.events[i-1].orchard_replay->next);
        Require(evicted.expired());
        const auto again=view->State(2);Require(again->Next()==trace.events[1].orchard_replay->next);
        Require(again->Parent()==trace.events[1].orchard_replay->parent&&view->State(2)==again);
        const auto original=view->State(1);
        Require(original->Nullifiers()==held->Nullifiers()&&original->Fees()==held->Fees());
        Require(original->Flows().size()==held->Flows().size());
        for(size_t i=0;i<held->Flows().size();++i){
            Require(original->Flows()[i].transparent_inputs==held->Flows()[i].transparent_inputs);
            Require(original->Flows()[i].transparent_outputs==held->Flows()[i].transparent_outputs);
            Require(original->Flows()[i].fee==held->Flows()[i].fee);
        }
    }
    Require(held->Next()==trace.events[0].orchard_replay->next&&!held->Nullifiers().empty());
    std::cout<<"RuntimeReplayBranchViews StateEvictionReload PASS\n";
}
void StateRetentionProvenance(const std::string& base) {
    using namespace wallet::detail;
    using Retention=RuntimeReplayStateRetention;
    static_assert(!std::is_default_constructible_v<Retention::Token>);
    Trace trace(base);const Branch origin{{},{},{},H(240),20000};
    const std::vector<VerifiedOrchardAuthorizations> shield{Authorized(base,false,20000)};
    (void)trace.Connect(origin,shield,81);const auto view=RuntimeAccountReplayTestAccess::Capture(trace.events);
    const auto verified=view->State(1);
    Retention owner,other;auto ticket=owner.Capture(*verified);const auto foreign=other.Capture(*verified);
    LookupReject(Status::Corruption,[&]{(void)owner.Load(foreign);});
    LookupReject(Status::Corruption,[&]{(void)other.Load(ticket);});
    auto& spool=RuntimeReplayStateRetentionTestAccess::Spool(owner);
    RuntimeReplaySpoolTestAccess::ChangeValueWithValidStorageMac(spool);
    LookupReject(Status::Corruption,[&]{(void)owner.Load(ticket);});
    // Another owner's untouched state remains valid, including after freeze.
    other.Freeze();const auto restored=other.Load(foreign);
    Require(restored->Parent()==verified->Parent()&&restored->Next()==verified->Next());
    Require(restored->Nullifiers()==verified->Nullifiers()&&restored->Fees()==verified->Fees());
    LookupReject(Status::Corruption,[&]{(void)other.Capture(*verified);});
    std::cout<<"RuntimeReplayBranchViews StateRetentionProvenance PASS\n";
}
void StateRetentionFailures(const std::string& base) {
    using namespace wallet::detail;
    Trace trace(base);const Branch origin{{},{},{},H(240),20000};
    (void)trace.Connect(origin,{},91);const auto view=RuntimeAccountReplayTestAccess::Capture(trace.events);
    const auto verified=view->State(1);RuntimeReplayStateRetention store;
    auto& spool=RuntimeReplayStateRetentionTestAccess::Spool(store);auto* db=RuntimeReplaySpoolTestAccess::Handle(spool);
    sqlite3_commit_hook(db,[](void*){return 1;},nullptr);
    bool refused=false;try{(void)store.Capture(*verified);}catch(const std::runtime_error&){refused=true;}
    Require(refused);sqlite3_commit_hook(db,nullptr,nullptr);
    const auto token=store.Capture(*verified); // Failed write did not publish a token or consume its slot.
    sqlite3_set_authorizer(db,[](void*,int action,const char*,const char*,const char*,const char*){return action==SQLITE_READ?SQLITE_DENY:SQLITE_OK;},nullptr);
    refused=false;try{(void)store.Load(token);}catch(const std::runtime_error&){refused=true;}
    Require(refused);sqlite3_set_authorizer(db,nullptr,nullptr);
    Require(store.Load(token)->Next()==verified->Next());
    std::cout<<"RuntimeReplayBranchViews StateRetentionFailures PASS\n";
}

void SameAuthorization(const VerifiedOrchardAuthorizations& a,const VerifiedOrchardAuthorizations& b){
    Require(a.Transaction().CanonicalBytes()==b.Transaction().CanonicalBytes());
    Require(a.Orchard().CanonicalBytes()==b.Orchard().CanonicalBytes());
    Require(a.Orchard().Txid()==b.Orchard().Txid()&&a.Orchard().Wtxid()==b.Orchard().Wtxid());
    Require(a.Orchard().Orchard().SigningDigest()==b.Orchard().Orchard().SigningDigest());
    Require(a.Transparent().OrchardIntent()==b.Transparent().OrchardIntent());
    Require(a.Transparent().CandidateHeight()==b.Transparent().CandidateHeight());
    const auto& x=a.Transparent().Snapshot();const auto& y=b.Transparent().Snapshot();
    Require(x.ViewHeight()==y.ViewHeight()&&x.Coins().size()==y.Coins().size());
    for(size_t i=0;i<x.Coins().size();++i){
        const auto& c=x.Coins()[i];const auto& d=y.Coins()[i];
        Require(c.value.GetUna()==d.value.GetUna()&&c.scriptPubKey==d.scriptPubKey&&c.height==d.height);
        Require(c.isCoinbase==d.isCoinbase&&c.is_confidential==d.is_confidential&&c.commitment==d.commitment);
    }
}
void AuthorizationEvictionReload(const std::string& base){
    // Generate one real payment to a known test wallet. This checks use of the
    // reloaded Rust handle by WalletNote, not merely equality of public bytes.
    auto keys=WalletKeys::FromSeed(std::array<uint8_t,64>{23},0);const auto fvk=keys.ExportFullViewingKey();
    Fixture fixture(base);fixture.outputs={{61000,fixture.view.coins.at(::Point(fixture.inputs[0])).scriptPubKey}};
    std::vector<ResolvedInput> resolved;
    for(const auto& in:fixture.inputs){const auto& coin=fixture.view.coins.at(::Point(in));
        resolved.push_back({in.txid_wire,in.output_index,in.sequence,coin.value.GetUna(),coin.scriptPubKey});}
    const std::vector<WalletPayment> payments{{5000,keys.Receiver(WalletScope::External,{})}};
    const auto proved=WalletBundlePlan::PrepareShield(keys,payments).Prove(
        SigningContext::Create(fixture.domain,fixture.lock,resolved,fixture.outputs,fixture.fee));
    fixture.bundle=proved.Bytes();fixture.Sign();
    const std::vector<VerifiedOrchardAuthorizations> shield{VerifyOrchardAuthorizations(fixture.Snapshot(),fixture.domain,20001,{})};
    Trace trace(base);Branch parent{{},{},{},H(240),20000};parent=trace.Connect(parent,shield,101);
    for(uint32_t i=0;i<7;++i)parent=trace.Connect(parent,{},102+i);
    RuntimeAccountReplay::AuthorizationHandle held;
    {
        const auto view=RuntimeAccountReplayTestAccess::Capture(trace.events);held=view->Authorizations(1);
        Require(held->size()==1&&view->Authorizations(1)==held);SameAuthorization(held->front(),shield.front());
        std::weak_ptr<const std::vector<VerifiedOrchardAuthorizations>> evicted=view->Authorizations(2);
        Require(!evicted.expired());for(uint64_t i=3;i<8;++i)Require(view->Authorizations(i)->empty());
        Require(evicted.expired());Require(view->Authorizations(2)->empty());
        SameAuthorization(view->Authorizations(1)->front(),shield.front());
    }
    SameAuthorization(held->front(),shield.front());bool received=false;
    const auto& restored=held->front().Orchard().Orchard();
    for(uint32_t i=0;i<restored.Facts().action_count;++i){
        const auto note=WalletNote::Receive(restored,fvk,WalletScope::External,i);
        if(note){Require(note->Facts().amount==5000);received=true;}
    }
    Require(received);std::cout<<"RuntimeReplayBranchViews AuthorizationEvictionReload PASS\n";
}
void AuthorizationRetentionProvenance(const std::string& base){
    using namespace wallet::detail;using Retention=RuntimeReplayAuthorizationRetention;
    static_assert(!std::is_default_constructible_v<Retention::Token>);
    const std::vector<VerifiedOrchardAuthorizations> verified{Authorized(base,false,20000),Authorized(base,true,20001)};
    Retention owner,other;const auto token=owner.Capture(verified),foreign=other.Capture(verified);
    LookupReject(Status::Corruption,[&]{(void)owner.Load(foreign);});
    LookupReject(Status::Corruption,[&]{(void)other.Load(token);});
    auto& spool=RuntimeReplayAuthorizationRetentionTestAccess::Spool(owner);
    RuntimeReplaySpoolTestAccess::ChangeValueWithValidStorageMac(spool,'a');
    LookupReject(Status::Corruption,[&]{(void)owner.Load(token);});
    other.Freeze();const auto restored=other.Load(foreign);Require(restored->size()==verified.size());
    for(size_t i=0;i<verified.size();++i)SameAuthorization((*restored)[i],verified[i]);
    LookupReject(Status::Corruption,[&]{(void)other.Capture(verified);});
    std::cout<<"RuntimeReplayBranchViews AuthorizationRetentionProvenance PASS\n";
}
void AuthorizationRetentionFailures(const std::string& base){
    using namespace wallet::detail;const std::vector<VerifiedOrchardAuthorizations> verified{Authorized(base,false,20000)};
    RuntimeReplayAuthorizationRetention store;auto& spool=RuntimeReplayAuthorizationRetentionTestAccess::Spool(store);
    auto* db=RuntimeReplaySpoolTestAccess::Handle(spool);sqlite3_commit_hook(db,[](void*){return 1;},nullptr);
    bool refused=false;try{(void)store.Capture(verified);}catch(const std::runtime_error&){refused=true;}
    Require(refused);sqlite3_commit_hook(db,nullptr,nullptr);const auto token=store.Capture(verified);
    sqlite3_set_authorizer(db,[](void*,int action,const char*,const char*,const char*,const char*){return action==SQLITE_READ?SQLITE_DENY:SQLITE_OK;},nullptr);
    refused=false;try{(void)store.Load(token);}catch(const std::runtime_error&){refused=true;}Require(refused);
    sqlite3_set_authorizer(db,nullptr,nullptr);const auto restored=store.Load(token);Require(restored->size()==1);
    SameAuthorization(restored->front(),verified.front());
    std::cout<<"RuntimeReplayBranchViews AuthorizationRetentionFailures PASS\n";
}

void RawSpoolCodec() {
    // Codec fidelity only: deliberately raw synthetic fields are not proof
    // authority. Actual signed replay validation remains in BranchMembership.
    RuntimeOutboxEvent event;event.cursor={9,H(2)};event.previous_digest=H(1);
    event.direction=RuntimeBlockDirection::Disconnect;event.context={31,H(3),H(4),30,{}};
    event.context.domain.network_code=2;event.context.domain.genesis_wire.fill(8);event.context.domain.branch_id=17;
    event.body={0,1,0,2,255};RuntimeOrchardReplay frame;
    frame.next={31,H(3),H(5),7,8,std::string("a\0b",3)};
    frame.parent=storage::OrchardStoredState{30,H(4),H(6),3,5,std::string("c\0d",3)};
    frame.coin_undo={0,9,0,10};frame.branch_mtp={{1,50},{20,60}};event.orchard_replay=frame;
    using namespace wallet::detail::replay_spool_codec;
    for(bool present:{true,false}) {
        if(!present)event.orchard_replay.reset();
        const auto encoded=Encode(event);Require(Encode(Decode(encoded))==encoded);
        for(size_t i=0;i<encoded.size();++i)LookupReject(Status::Corruption,[&]{(void)Decode(std::span<const uint8_t>(encoded.data(),i));});
        auto extra=encoded;extra.push_back(0);LookupReject(Status::Corruption,[&]{(void)Decode(extra);});
        auto wrong_direction=encoded;wrong_direction[73]=3;LookupReject(Status::Corruption,[&]{(void)Decode(wrong_direction);});
    }
    std::cout<<"RuntimeReplayBranchViews RawSpoolCodec PASS\n";
}
}
int main(int argc,char** argv) {
    try { Require(argc==2);BranchMembership(argv[1]);RawSpoolCodec();EventEvictionReload(argv[1]);BodyEvictionReload(argv[1]);HistoricalBodyEvictionReload(argv[1]);StateEvictionReload(argv[1]);StateRetentionProvenance(argv[1]);StateRetentionFailures(argv[1]);AuthorizationEvictionReload(argv[1]);AuthorizationRetentionProvenance(argv[1]);AuthorizationRetentionFailures(argv[1]); }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    return 0;
}
