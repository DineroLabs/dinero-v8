#pragma once
#include "p2p/orchard_catalog_transfer.h"
#include "network/types.h"
#include <chrono>
#include <thread>

namespace dinero::p2p {
// Single serialized download actor. Its host owns scheduling/peer selection
// and uses the real steady clock. This actor grants NO live-owner, scan-source
// or wallet authority: even a finished section is only unpublished proof data.
class OrchardCatalogClient final {
public:
    using Transfer=OrchardCatalogTransfer;using Bytes=Transfer::Bytes;
    using Clock=std::chrono::steady_clock;using Time=Clock::time_point;
    struct Peer {std::string address;uint64_t session=0,services=0;};
    struct Outbound {Peer peer;std::string command;Bytes bytes;};
    enum class State { Idle, Manifest, Pages, Ready, Failed, Cancelled, Taken };
    OrchardCatalogClient(Transfer::Request request,std::shared_ptr<const consensus::HeaderChainSelector> headers,
        std::filesystem::path work,std::function<bool()> cancelled={}):request_(std::move(request)),
        headers_(std::move(headers)),work_(std::move(work)),cancelled_(std::move(cancelled)) {
        Require(headers_&&!work_.empty()&&std::filesystem::is_directory(work_));request_.CheckSelected(*headers_);
    }
    OrchardCatalogClient(const OrchardCatalogClient&)=delete;
    OrchardCatalogClient& operator=(const OrchardCatalogClient&)=delete;
    bool Begin(Peer peer,Time now){
        CheckThread();if(state_!=State::Idle||!peer.session||peer.address.empty()||peer.address.size()>256||
            !(peer.services&ServiceFlags::NODE_ORCHARD_CATALOG_V1))return false;
        request_.CheckSelected(*headers_);peer_=std::move(peer);began_=received_=sent_=window_=last_=now;
        state_=State::Manifest;return true;
    }
    State Current()const{CheckThread();return state_;}
    // No automatic reuse/restart. A failed actor's host must enforce peer
    // rotation/cooldown and create a fresh nonce/actor if still locally needed.
    Time RetryAfter()const{CheckThread();return last_+std::chrono::minutes(1);}
    void Cancel(){CheckThread();receiver_.reset();proof_.reset();state_=State::Cancelled;awaiting_=false;}
    std::optional<Outbound> Next(Time now){
        CheckThread();if(!Live())return {};if(!Fresh(now))return {};
        try {
            request_.CheckSelected(*headers_);
            if(now-window_>=std::chrono::seconds(1)){window_=now;pages_=0;}
            if(state_==State::Pages&&pages_>=16)return {};
            if(awaiting_){
                // One byte-identical page retry; no duplicate preparation
                // request, unlimited retransmission or moving idle deadline.
                if(state_!=State::Pages||retried_||now-sent_<std::chrono::seconds(15))return {};
                retried_=true;sent_=now;++pages_;return Outbound{peer_,Transfer::PageRequestCommand,outstanding_};
            }
            if(state_==State::Manifest){awaiting_=true;sent_=now;return Outbound{peer_,Transfer::RequestCommand,request_.Encode()};}
            outstanding_=receiver_->RequestNext();++pages_;awaiting_=true;retried_=false;sent_=now;
            return Outbound{peer_,Transfer::PageRequestCommand,outstanding_};
        } catch(...){Fail();return {};}
    }
    bool Receive(const Peer& peer,const std::string& command,std::span<const uint8_t> bytes,Time now){
        CheckThread();
        // Late/unsolicited packets from a different connection cannot poison
        // the selected flight or select another connection with the same name.
        if(!Live()||peer.address!=peer_.address||peer.session!=peer_.session)return false;
        if(!Fresh(now))return false;
        try {
            // A cancelled/expired flight may finish on this same connection.
            // Ignore its nonce/transfer ID without acknowledging our flight or
            // extending its deadline. Matching-flight corruption still fails.
            if(command==Transfer::ManifestCommand&&bytes.size()==195){
                const auto manifest=Transfer::Manifest::Decode(bytes);
                if(manifest.request.nonce!=request_.nonce)return false;
            }
            if(command==Transfer::PageCommand&&bytes.size()>=40&&bytes.size()<=40+Transfer::PageBytes&&state_==State::Pages){
                if(!std::equal(transfer_id_.begin(),transfer_id_.end(),bytes.begin()))return false;
            }
            Require(awaiting_);request_.CheckSelected(*headers_);
            if(state_==State::Manifest){
                Require(command==Transfer::ManifestCommand&&bytes.size()==195);
                const auto manifest=Transfer::Manifest::Decode(bytes);
                Require(manifest.request==request_);
                // Receiver keeps transfer pages plus decoded immutable nodes.
                // Reserve a conservative disk floor before allocating either;
                // the codec still enforces its independent 1GiB section cap.
                const auto space=std::filesystem::space(work_);
                Require(space.available>=2*manifest.bytes+(uint64_t(512)<<20));
                receiver_=std::make_unique<Transfer::Receiver>(request_,bytes,*headers_,work_);
                total_pages_=manifest.Pages();transfer_id_=manifest.Id();state_=State::Pages;
                // The server's per-session window starts when it sends the
                // manifest, not when we requested potentially slow preparation.
                // Start no earlier than receipt, avoiding a quota-edge drop
                // and a needless 15-second retry at every window boundary.
                window_=now;pages_=0;
            } else {
                Require(command==Transfer::PageCommand&&bytes.size()>=40&&bytes.size()<=40+Transfer::PageBytes);
                const auto wanted=Transfer::PageRequest::Decode(outstanding_).index;
                uint32_t index=0;for(unsigned i=0;i<4;++i)index|=uint32_t(bytes[32+i])<<(8*i);
                // A delayed reply to our one retry may trail the first reply.
                // It must not acknowledge the new flight or extend idleness.
                if(wanted&&index==wanted-1)return false;
                Require(index==wanted);
                receiver_->Accept(bytes);
                if(receiver_->NextPage()==total_pages_){
                    proof_=std::make_shared<const consensus::OrchardSnapshotSection>(receiver_->Finish(*headers_,cancelled_));
                    receiver_.reset();state_=State::Ready;
                }
            }
            received_=now;awaiting_=false;return true;
        } catch(...){Fail();return false;}
    }
    // A captured immutable proof, not an installer capability. Handoff must
    // recheck its current selected/durable source and all before-images.
    std::shared_ptr<const consensus::OrchardSnapshotSection> Take(){
        CheckThread();Require(state_==State::Ready);
        try{request_.CheckSelected(*headers_);}catch(...){Fail();throw;}
        state_=State::Taken;return std::move(proof_);
    }
private:
    static void Require(bool ok){if(!ok)throw std::runtime_error("Orchard catalog client refused");}
    void CheckThread()const{Require(thread_==std::this_thread::get_id());}
    bool Live()const{return state_==State::Manifest||state_==State::Pages;}
    void Fail(){receiver_.reset();proof_.reset();awaiting_=false;state_=State::Failed;}
    bool Fresh(Time now){
        // First archival preparation can consume the server's ten-minute
        // budget. It allocates no receiver at the client. Once pages begin,
        // retain the short idle bound. 1GiB at sixteen 64KiB pages/sec alone
        // takes >17min; preparation plus transfer must fit the total budget.
        const auto idle=state_==State::Manifest?std::chrono::seconds(660):std::chrono::seconds(30);
        if((cancelled_&&cancelled_())||now<last_||now-began_>=std::chrono::minutes(45)||now-received_>=idle){Fail();return false;}
        last_=now;return true;
    }
    const Transfer::Request request_;const std::shared_ptr<const consensus::HeaderChainSelector> headers_;
    const std::filesystem::path work_;const std::function<bool()> cancelled_;
    const std::thread::id thread_=std::this_thread::get_id();
    Peer peer_;State state_=State::Idle;Time began_{},received_{},sent_{},window_{},last_{};
    std::unique_ptr<Transfer::Receiver> receiver_;std::shared_ptr<const consensus::OrchardSnapshotSection> proof_;
    Bytes outstanding_;uint256 transfer_id_;uint32_t total_pages_=0,pages_=0;bool awaiting_=false,retried_=false;
};
} // namespace dinero::p2p
