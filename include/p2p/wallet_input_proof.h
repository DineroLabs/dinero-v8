#pragma once
#include "p2p/orchard_catalog_client.h"
#include "crypto/secure_random.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/utreexo_stump.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace dinero::p2p {
// PUBLIC proof DATA, never a coin view, chain selection, wallet identity or
// reservation capability. The consumer must bind the exact local selected
// catalog and authenticate legacy metadata before using a path.
struct WalletInputProof {
    using Bytes=std::vector<uint8_t>;
    static constexpr const char* RequestCommand="getwinput";
    static constexpr const char* ReplyCommand="winput";
    static constexpr size_t MaximumBytes=16*1024,RequestBytes=6+4+32+4*4+32*4+4;
    static uint256 KeyNonce(){uint256 v;v.begin()[0]=1;return v;}
    struct Request {
        uint32_t magic=0;uint256 genesis;uint32_t branch=0,activation=0,leaf_activation=0,height=0;
        uint256 tip,root,nonce;OutPoint point;
        bool operator==(const Request&)const=default;
        Bytes Encode()const {
            Check(!genesis.IsNull()&&branch&&activation&&activation!=UINT32_MAX&&height>=activation&&
                !tip.IsNull()&&!root.IsNull()&&!nonce.IsNull()&&!point.txid.IsNull());
            Bytes b{'D','N','W','I','P',1};Put(b,magic,4);Hash(b,genesis);
            for(auto n:{branch,activation,leaf_activation,height})Put(b,n,4);
            Hash(b,tip);Hash(b,root);Hash(b,nonce);Hash(b,point.txid.AsUint256());Put(b,point.vout,4);return b;
        }
        static Request Decode(std::span<const uint8_t> bytes){
            Reader r(bytes);Check(bytes.size()==RequestBytes);r.Tag();Request v;
            v.magic=r.Number(4);v.genesis=r.Hash();v.branch=r.Number(4);v.activation=r.Number(4);
            v.leaf_activation=r.Number(4);v.height=r.Number(4);v.tip=r.Hash();v.root=r.Hash();v.nonce=r.Hash();
            v.point=OutPoint(TxId(r.Hash()),uint32_t(r.Number(4)));r.End();Check(v.Encode()==Bytes(bytes.begin(),bytes.end()));return v;
        }
        void CheckProfile()const {
            const auto& p=Params();Check(consensus::OrchardProfileConfigurationValid(p)&&magic==p.magic&&
                genesis==uint256::FromHexUnsafe(p.genesis_hash)&&branch==p.orchard_branch_id&&
                activation==p.orchard_activation_height&&leaf_activation==consensus::GetUtreexoMaturityLeafActivationHeight());
            (void)Encode();
        }
    };
    struct Reply {
        Request request;uint64_t value=0;uint32_t created=0;bool coinbase=false;
        Bytes script;consensus::UtreexoProof path;
        Bytes Encode()const {
            auto b=request.Encode();Check(value&&value<=orchard::kMaxMoneyUna&&created<=request.height&&
                !script.empty()&&script.size()<=10000&&path.siblings.size()<=63);
            const auto proof=path.serialize();Put(b,value,8);Put(b,created,4);Put(b,coinbase,1);
            Put(b,script.size(),4);b.insert(b.end(),script.begin(),script.end());Put(b,proof.size(),4);
            b.insert(b.end(),proof.begin(),proof.end());Check(b.size()<=MaximumBytes);return b;
        }
        static Reply Decode(std::span<const uint8_t> bytes){
            Check(bytes.size()>RequestBytes&&bytes.size()<=MaximumBytes);Reply v;v.request=Request::Decode(bytes.first(RequestBytes));
            Reader r(bytes.subspan(RequestBytes));v.value=r.Number(8);v.created=r.Number(4);const auto flag=r.Number(1);Check(flag<=1);v.coinbase=flag;
            v.script=r.BytesValue(r.Number(4),10000);const auto raw=r.BytesValue(r.Number(4),4096);
            v.path=consensus::UtreexoProof::deserialize(raw);r.End();Check(v.Encode()==Bytes(bytes.begin(),bytes.end()));return v;
        }
        // Legacy height/coinbase are NOT authenticated by this check alone.
        bool VerifyPath(const consensus::UtreexoStump& stump)const {
            const auto root=stump.getCommitment();
            if(root.size()!=32||!std::equal(root.begin(),root.end(),request.root.begin())||path.numLeaves!=stump.getNumLeaves())return false;
            const auto leaf=consensus::HashUTXOForCreationHeight(request.point.txid.AsUint256(),request.point.vout,value,script,created,coinbase);
            return path.verify(leaf,stump.getRoots());
        }
    };
    static void Check(bool good){if(!good)throw std::invalid_argument("Wallet input proof DATA refused");}
    static void Put(Bytes& b,uint64_t n,unsigned size){for(unsigned i=0;i<size;++i)b.push_back(uint8_t(n>>(8*i)));}
    static void Hash(Bytes& b,const uint256& h){b.insert(b.end(),h.begin(),h.end());}
    class Reader {
        std::span<const uint8_t> bytes_;size_t at_=0;
    public:
        explicit Reader(std::span<const uint8_t> b):bytes_(b){}
        Bytes BytesValue(uint64_t n,size_t maximum){Check(n<=maximum&&n<=bytes_.size()-at_);Bytes v(bytes_.begin()+at_,bytes_.begin()+at_+n);at_+=n;return v;}
        uint64_t Number(unsigned n){const auto b=BytesValue(n,8);uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[i])<<(8*i);return v;}
        uint256 Hash(){const auto b=BytesValue(32,32);uint256 h;std::copy(b.begin(),b.end(),h.begin());return h;}
        void Tag(){Check(BytesValue(6,6)==Bytes({'D','N','W','I','P',1}));}
        void End(){Check(at_==bytes_.size());}
    };
};

// One exact-session client flight; eight wanted/cached proofs; no wallet or
// selected lock on the worker. Failed peers cool down, never gain authority.
class WalletInputProofDownload final {
public:
    using Wire=WalletInputProof;using Bytes=Wire::Bytes;using Peer=OrchardCatalogClient::Peer;
    static constexpr size_t MaximumItems=8;
    using Peers=std::function<std::vector<Peer>()>;using Send=std::function<bool(const Peer&,const std::string&,const Bytes&)>;
    WalletInputProofDownload(Peers peers,Send send):peers_(std::move(peers)),send_(std::move(send)),worker_([this]{Run();}){}
    ~WalletInputProofDownload(){Stop();}
    std::shared_ptr<const Wire::Reply> Request(Wire::Request request){
        request.nonce=Wire::KeyNonce();request.CheckProfile();const auto key=request.Encode();
        std::lock_guard lock(mutex_);if(stop_)return {};
        if(!wanted_.empty()&&(wanted_.front().tip!=request.tip||wanted_.front().root!=request.root||wanted_.front().height!=request.height)){
            // A no-longer-selected head must not block the current request.
            // Generation checks prevent an already copied old packet from
            // publishing into this new flight/cache after cancellation.
            if(generation_==UINT64_MAX)throw std::runtime_error("Input source generation exhausted");
            ++generation_;wanted_.clear();ready_.clear();flight_.reset();packet_.reset();
        }
        if(const auto found=ready_.find(key);found!=ready_.end())return found->second;
        if(std::none_of(wanted_.begin(),wanted_.end(),[&](const auto& v){return v.Encode()==key;})){
            if(wanted_.size()>=MaximumItems)throw std::runtime_error("Wallet input proof request capacity exceeded");
            wanted_.push_back(std::move(request));cv_.notify_one();}
        return {};
    }
    bool Enqueue(const Peer& peer,std::span<const uint8_t> bytes){
        if(bytes.size()<=Wire::RequestBytes||bytes.size()>Wire::MaximumBytes)return false;
        std::lock_guard lock(mutex_);
        if(stop_||!flight_||packet_||peer.address!=flight_->address||peer.session!=flight_->session)return false;
        packet_=Bytes(bytes.begin(),bytes.end());cv_.notify_one();return true;
    }
    void Stop(){std::lock_guard join(join_);{std::lock_guard lock(mutex_);stop_=true;packet_.reset();flight_.reset();wanted_.clear();ready_.clear();}
        cv_.notify_all();if(worker_.joinable())worker_.join();}
private:
    using Clock=std::chrono::steady_clock;const Peers peers_;const Send send_;
    std::mutex mutex_,join_;std::condition_variable cv_;bool stop_=false;uint64_t generation_=0;
    std::deque<Wire::Request> wanted_;std::map<Bytes,std::shared_ptr<const Wire::Reply>> ready_;
    std::optional<Peer> flight_;std::optional<Bytes> packet_;std::thread worker_;
    void Run()noexcept{
        std::map<std::string,Clock::time_point> cooldown;Clock::time_point sent{},window=Clock::now();size_t used=0;
        std::optional<Wire::Request> sent_request;
        for(;;){
            std::optional<Peer> peer;std::optional<Bytes> packet;Wire::Request request;uint64_t generation=0;
            {std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(50),[&]{return stop_||packet_.has_value();});
             if(stop_)return;if(wanted_.empty())continue;generation=generation_;request=wanted_.front();peer=flight_;packet=std::move(packet_);packet_.reset();}
            const auto fail=[&]{std::lock_guard lock(mutex_);if(generation==generation_){
                if(peer){if(cooldown.size()>=32)cooldown.erase(cooldown.begin());cooldown[peer->address]=Clock::now()+std::chrono::minutes(1);}
                flight_.reset();packet_.reset();}sent_request.reset();};
            try{
                const auto now=Clock::now();const auto connected=peers_();
                if(packet){used+=packet->size();const auto reply=Wire::Reply::Decode(*packet);
                    Wire::Check(sent_request&&reply.request==*sent_request);auto key=request.Encode();
                    auto data=std::make_shared<const Wire::Reply>(reply);std::lock_guard lock(mutex_);if(stop_)return;
                    if(generation!=generation_)continue;
                    if(ready_.size()>=MaximumItems)ready_.erase(ready_.begin());ready_.emplace(std::move(key),std::move(data));
                    wanted_.pop_front();flight_.reset();sent_request.reset();continue;}
                if(peer){if(now-sent>=std::chrono::seconds(30)||!std::any_of(connected.begin(),connected.end(),[&](const auto& p){return p.address==peer->address&&p.session==peer->session;}))fail();continue;}
                for(auto i=cooldown.begin();i!=cooldown.end();)if(i->second<=now)i=cooldown.erase(i);else ++i;
                if(now-window>=std::chrono::minutes(1)){window=now;used=0;}if(used>=1024*1024)continue;
                for(const auto& p:connected)if(p.session&&!p.address.empty()&&p.address.size()<=256&&(p.services&ServiceFlags::NODE_NETWORK)&&!cooldown.contains(p.address)){peer=p;break;}
                if(!peer)continue;const auto random=secure_random_bytes(32);std::copy(random.begin(),random.end(),request.nonce.begin());
                const auto bytes=request.Encode();{std::lock_guard lock(mutex_);if(stop_)return;if(generation!=generation_)continue;flight_=peer;sent_request=request;}
                sent=now;if(!send_(*peer,Wire::RequestCommand,bytes))fail();
            }catch(...){fail();}
        }
    }
};

// Bounded asynchronous FULL source. Capture/proving never runs on P2P ingress.
// At most four packets, 32 address budgets, eight requests/address/minute and
// 64 requests globally/minute, including invalid requests. Stop drains source.
class WalletInputProofServer final {
public:
    using Wire=WalletInputProof;using Peer=OrchardCatalogClient::Peer;using Bytes=Wire::Bytes;
    using Serve=std::function<std::optional<Wire::Reply>(const Wire::Request&)>;
    using Send=WalletInputProofDownload::Send;
    WalletInputProofServer(Serve serve,Send send):serve_(std::move(serve)),send_(std::move(send)),worker_([this]{Run();}){}
    ~WalletInputProofServer(){Stop();}
    bool Enqueue(const Peer& peer,std::span<const uint8_t> bytes){
        if(bytes.size()!=Wire::RequestBytes||!peer.session||peer.address.empty()||peer.address.size()>256)return false;
        std::lock_guard lock(mutex_);if(stop_||queue_.size()>=4)return false;
        const auto now=std::chrono::steady_clock::now();if(now-window_>=std::chrono::minutes(1)){window_=now;budget_.clear();used_=0;}
        if(used_>=64||(!budget_.contains(peer.address)&&budget_.size()>=32))return false;
        auto& used=budget_[peer.address];if(used>=8)return false;++used;++used_;
        queue_.push_back({peer,Bytes(bytes.begin(),bytes.end())});cv_.notify_one();return true;
    }
    void Stop(){std::lock_guard join(join_);{std::lock_guard lock(mutex_);stop_=true;queue_.clear();}cv_.notify_all();if(worker_.joinable())worker_.join();}
private:
    const Serve serve_;const Send send_;std::mutex mutex_,join_;std::condition_variable cv_;bool stop_=false;
    std::deque<std::pair<Peer,Bytes>> queue_;std::map<std::string,size_t> budget_;size_t used_=0;
    std::chrono::steady_clock::time_point window_=std::chrono::steady_clock::now();std::thread worker_;
    void Run()noexcept{for(;;){std::pair<Peer,Bytes> packet;
        {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stop_||!queue_.empty();});if(stop_)return;packet=std::move(queue_.front());queue_.pop_front();}
        try{const auto request=Wire::Request::Decode(packet.second);request.CheckProfile();const auto reply=serve_(request);
            if(!reply)continue;Wire::Check(reply->request==request);const auto bytes=reply->Encode();
            {std::lock_guard lock(mutex_);if(stop_)return;}(void)send_(packet.first,Wire::ReplyCommand,bytes);
        }catch(...){} }
    }
};
}
