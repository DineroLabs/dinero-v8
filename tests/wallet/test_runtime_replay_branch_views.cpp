#include "../consensus/orchard_block_test_fixture.h"
#include "consensus/undo.h"
#include "wallet/runtime_account_replay.h"
#include "wallet/runtime_replay_spool_codec.h"
#include <set>

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
    try { Require(argc==2);BranchMembership(argv[1]);RawSpoolCodec();EventEvictionReload(argv[1]); }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    return 0;
}
