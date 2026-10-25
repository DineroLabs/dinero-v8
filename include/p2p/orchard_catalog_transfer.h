#pragma once

#include "consensus/orchard_snapshot.h"
#include "consensus/orchard_profile.h"
#include "consensus/header_chain.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "daemon/orchard_snapshot_store.h"
#include "wallet/runtime_replay_spool.h"
#include <cstring>

namespace dinero::p2p {
// Boundary-only, versioned transfer framing. It reuses the DNORST02 proof
// section, NOT the v6 file/bootstrap API: no filename, UTXO set, birthday,
// imported validity flags or peer-supplied chain selection exist on this wire.
// All decoded data remains untrusted. This type cannot publish a chainstate,
// create a compact owner, issue wallet replay authority or clear a boundary.
class OrchardCatalogTransfer final {
public:
    using Bytes=std::vector<uint8_t>;
    static constexpr uint32_t PageBytes=64*1024;
    static constexpr uint64_t MaximumBytes=consensus::ORCHARD_SNAPSHOT_MAX_SECTION_BYTES;
    static constexpr const char* RequestCommand="getocat";
    static constexpr const char* ManifestCommand="ocatmeta";
    static constexpr const char* PageRequestCommand="getocatpg";
    static constexpr const char* PageCommand="ocatpage";
    struct Request {
        uint32_t magic=0;uint8_t network=255;
        uint256 genesis,nonce,parent,base;
        uint32_t branch=0,activation=0,leaf_activation=0;
        bool operator==(const Request&)const=default;
        Bytes Encode()const {
            Check(network<=2&&!genesis.IsNull()&&!nonce.IsNull()&&!parent.IsNull()&&!base.IsNull()&&
                branch&&activation&&activation!=UINT32_MAX);
            Bytes b{'D','N','O','C','T',1};Put(b,magic,4);Put(b,network,1);
            Hash(b,genesis);Hash(b,nonce);Put(b,branch,4);Put(b,activation,4);Put(b,leaf_activation,4);
            Hash(b,parent);Hash(b,base);return b;
        }
        static Request Decode(std::span<const uint8_t> bytes){
            Reader r(bytes);r.Tag("DNOCT",1);Request v;v.magic=r.Number(4);v.network=r.Number(1);
            v.genesis=r.Hash();v.nonce=r.Hash();v.branch=r.Number(4);v.activation=r.Number(4);v.leaf_activation=r.Number(4);
            v.parent=r.Hash();v.base=r.Hash();r.End();Check(v.Encode()==Bytes(bytes.begin(),bytes.end()));return v;
        }
        // The operator cannot select a different activation, peer header or
        // unburied base. Returning a request is still not state authority.
        static Request Selected(const consensus::HeaderChainSelector& headers,const uint256& nonce){
            const auto& p=Params();Check(consensus::OrchardProfileConfigurationValid(p)&&
                p.orchard_catalog_commitment_activation_height==p.orchard_activation_height&&
                p.orchard_activation_height&&p.orchard_activation_height!=UINT32_MAX);
            consensus::HeaderIndexEntry base,parent;
            Check(headers.GetHeaderAtHeightCopy(p.orchard_activation_height,base)&&
                headers.GetHeaderAtHeightCopy(p.orchard_activation_height-1,parent));
            Request r{p.magic,uint8_t(p.name=="mainnet"?0:p.name=="testnet"?1:2),
                uint256::FromHexUnsafe(p.genesis_hash),nonce,parent.hash,base.hash,
                p.orchard_branch_id,p.orchard_activation_height,consensus::GetUtreexoMaturityLeafActivationHeight()};
            r.CheckSelected(headers);return r;
        }
        void CheckSelected(const consensus::HeaderChainSelector& headers)const {
            (void)Encode();const auto& p=Params();
            Check(consensus::OrchardProfileConfigurationValid(p)&&magic==p.magic&&
                network==uint8_t(p.name=="mainnet"?0:p.name=="testnet"?1:2)&&genesis==uint256::FromHexUnsafe(p.genesis_hash)&&
                branch==p.orchard_branch_id&&activation==p.orchard_activation_height&&
                activation==p.orchard_catalog_commitment_activation_height&&
                leaf_activation==consensus::GetUtreexoMaturityLeafActivationHeight());
            const auto best=headers.GetBestHeaderValue();consensus::HeaderIndexEntry b,a;
            Check(best&&headers.GetHeaderAtHeightCopy(activation,b)&&b.hash==base&&b.prev_hash==parent&&
                headers.GetHeaderAtHeightCopy(activation-1,a)&&a.hash==parent&&a.chainwork<b.chainwork);
            uint256 ancestor;uint32_t height=0;
            Check(uint64_t(best->height)>=uint64_t(activation)+p.state_commitment_burial_depth&&
                headers.GetAncestorHashByHash(best->hash,activation,ancestor,height)&&height==best->height&&ancestor==base);
        }
    };
    struct Manifest {
        Request request;uint64_t bytes=0;uint256 digest;
        Bytes Encode()const {
            Check(bytes&&bytes<=MaximumBytes&&!digest.IsNull());auto b=request.Encode();
            Put(b,bytes,8);Put(b,PageBytes,4);Hash(b,digest);return b;
        }
        uint256 Id()const {const auto b=Encode();return Digest(b);}
        uint32_t Pages()const {Check(bytes&&bytes<=MaximumBytes);return uint32_t((bytes+PageBytes-1)/PageBytes);}
        static Manifest Decode(std::span<const uint8_t> bytes){
            constexpr size_t request_bytes=151;Check(bytes.size()==request_bytes+44);
            Manifest m;m.request=Request::Decode(bytes.first(request_bytes));Reader r(bytes.subspan(request_bytes));
            m.bytes=r.Number(8);Check(r.Number(4)==PageBytes);m.digest=r.Hash();r.End();(void)m.Encode();return m;
        }
    };
    struct PageRequest {
        uint256 transfer;uint32_t index=0;
        Bytes Encode()const {
            Check(!transfer.IsNull()&&index<MaximumBytes/PageBytes);
            Bytes b{'D','N','O','P','G',1};Hash(b,transfer);Put(b,index,4);return b;
        }
        static PageRequest Decode(std::span<const uint8_t> bytes){
            Reader r(bytes);r.Tag("DNOPG",1);PageRequest p;p.transfer=r.Hash();p.index=r.Number(4);r.End();
            Check(p.Encode()==Bytes(bytes.begin(),bytes.end()));return p;
        }
    };
    class Sender {
    public:
        Sender(const Request& request,const consensus::OrchardSnapshotSection& proof,const std::filesystem::path& work={},
            const std::function<bool()>& cancelled={})
            :spool_(std::make_shared<wallet::detail::RuntimeReplaySpool>(work,true)) {
            const auto check_cancel=[&]{Check(!cancelled||!cancelled());};check_cancel();
            CheckProofBinding(request,proof);manifest_.request=request;
            // Structural walk is mandatory even for internally captured proof
            // data. The actual selected-chain verifier is a separate gate.
            consensus::ValidateOrchardSnapshotSection(proof);
            check_cancel();
            Bytes pending;pending.reserve(PageBytes);uint32_t page=0;crypto::CSHA256 hash;
            wallet::detail::RuntimeReplaySpool::Batch batch(*spool_);
            manifest_.bytes=consensus::StreamOrchardSnapshotSection(proof,[&](auto bytes){
                check_cancel();
                hash.Write(bytes.data(),bytes.size());
                while(!bytes.empty()){
                    const auto n=std::min<size_t>(PageBytes-pending.size(),bytes.size());
                    pending.insert(pending.end(),bytes.begin(),bytes.begin()+n);bytes=bytes.subspan(n);
                    if(pending.size()==PageBytes){spool_->Insert(Key(page++),pending);pending.clear();}
                }
            });
            if(!pending.empty())spool_->Insert(Key(page++),pending);
            hash.Finalize(manifest_.digest.data);Check(page==manifest_.Pages());batch.Commit();spool_->Freeze();
            check_cancel();
        }
        // One immutable disk payload serves bounded peer sessions. Only their
        // nonce/manifest ID differ; no repeat catalog walk or section copy.
        Sender Rebind(const Request& request)const {
            (void)request.Encode();auto expected=manifest_.request;expected.nonce=request.nonce;
            Check(expected==request);Sender result(*this);result.manifest_.request=request;return result;
        }
        const Manifest& Description()const{return manifest_;}
        Bytes Serve(std::span<const uint8_t> request)const {
            const auto page=PageRequest::Decode(request);Check(page.transfer==manifest_.Id());return Page(page.index);
        }
        Bytes Page(uint32_t index)const {
            Check(index<manifest_.Pages());const auto bytes=spool_->Get(Key(index));Check(bool(bytes));
            Check(bytes->size()==PageSize(manifest_,index));Bytes b;Hash(b,manifest_.Id());Put(b,index,4);
            Put(b,bytes->size(),4);b.insert(b.end(),bytes->begin(),bytes->end());return b;
        }
    private:std::shared_ptr<wallet::detail::RuntimeReplaySpool> spool_;Manifest manifest_;
    };
    class Receiver {
    public:
        Receiver(const Request& expected,std::span<const uint8_t> manifest,
                 const consensus::HeaderChainSelector& headers,const std::filesystem::path& work={})
            :manifest_(Screen(expected,manifest,headers)),spool_(work,true),spool_work_(work){}
        void Accept(std::span<const uint8_t> page){
            const bool usable=!poisoned_&&!finished_;poisoned_=true;Check(usable);
            Check(page.size()>=40&&page.size()<=40+PageBytes);Reader r(page);
            Check(r.Hash()==manifest_.Id());const auto index=r.Number(4),size=r.Number(4);
            Check(index<manifest_.Pages()&&size==PageSize(manifest_,uint32_t(index))&&page.size()==40+size);
            const auto payload=page.subspan(40);
            // A single sequential flight bounds untrusted work and state.
            // Only the immediately preceding byte-identical retry is harmless.
            if(next_&&index==next_-1){
                const auto old=spool_.Get(Key(uint32_t(index)));Check(old&&*old==Bytes(payload.begin(),payload.end()));
            }else{
                Check(index==next_);wallet::detail::RuntimeReplaySpool::Batch batch(spool_);
                spool_.Insert(Key(next_),payload);batch.Commit();hash_.Write(payload.data(),payload.size());++next_;
            }
            poisoned_=false;
        }
        uint32_t NextPage()const {Check(!poisoned_&&!finished_);return next_;}
        Bytes RequestNext()const {
            Check(!poisoned_&&!finished_&&next_<manifest_.Pages());return PageRequest{manifest_.Id(),next_}.Encode();
        }
        // Returns UNPUBLISHED proof material. Chainstate installation must
        // reauthenticate against its selected source under exclusive ownership.
        // The network checksum, transport digest and disk MAC grant no authority.
        consensus::OrchardSnapshotSection Finish(const consensus::HeaderChainSelector& headers,
            const std::function<bool()>& cancelled={}){
            const bool usable=!poisoned_&&!finished_;poisoned_=true;Check(usable&&next_==manifest_.Pages());
            const auto check_cancel=[&]{Check(!cancelled||!cancelled());};check_cancel();
            manifest_.request.CheckSelected(headers);uint256 hash;hash_.Finalize(hash.data);Check(hash==manifest_.digest);
            spool_.Freeze();uint64_t offset=0;Bytes cached;uint32_t cached_index=UINT32_MAX;
            auto proof=consensus::ReadOrchardSnapshotSection(manifest_.bytes,[&](std::span<uint8_t> out){
                check_cancel();
                Check(out.size()<=manifest_.bytes-offset);
                while(!out.empty()){
                    const auto index=uint32_t(offset/PageBytes),at=uint32_t(offset%PageBytes);
                    if(index!=cached_index){const auto row=spool_.Get(Key(index));Check(row.has_value());cached=*row;cached_index=index;}
                    Check(at<cached.size());const auto n=std::min(out.size(),cached.size()-at);
                    std::copy_n(cached.begin()+at,n,out.begin());out=out.subspan(n);offset+=n;
                }
            },spool_work_);
            Check(offset==manifest_.bytes);CheckProofBinding(manifest_.request,proof);
            check_cancel();Authenticate(proof,headers);check_cancel();
            manifest_.request.CheckSelected(headers);finished_=true;return proof;
        }
    private:
        static Manifest Screen(const Request& expected,std::span<const uint8_t> bytes,const consensus::HeaderChainSelector& headers){
            auto m=Manifest::Decode(bytes);Check(m.request==expected);expected.CheckSelected(headers);return m;
        }
        const Manifest manifest_;wallet::detail::RuntimeReplaySpool spool_;
        const std::filesystem::path spool_work_;crypto::CSHA256 hash_;uint32_t next_=0;bool poisoned_=false,finished_=false;
    };
private:
    static void Authenticate(const consensus::OrchardSnapshotSection& proof,const consensus::HeaderChainSelector& headers){
        OrchardSnapshotStore::VerifyCatalogTransferProof(proof,headers);
    }
    static void Check(bool ok){if(!ok)throw std::runtime_error("Orchard catalog transfer refused");}
    static void Put(Bytes& b,uint64_t n,unsigned size){for(unsigned i=0;i<size;++i){b.push_back(uint8_t(n));n>>=8;}}
    static void Hash(Bytes& b,const uint256& h){b.insert(b.end(),h.begin(),h.end());}
    static uint256 Digest(std::span<const uint8_t> b){uint256 h;crypto::CSHA256().Write(b.data(),b.size()).Finalize(h.data);return h;}
    static Bytes Key(uint32_t page){Bytes b{1};Put(b,page,4);return b;}
    static size_t PageSize(const Manifest& m,uint32_t index){Check(index<m.Pages());return size_t(std::min<uint64_t>(PageBytes,m.bytes-uint64_t(index)*PageBytes));}
    static void CheckProofBinding(const Request& r,const consensus::OrchardSnapshotSection& s){
        (void)r.Encode();Check(s.profile==consensus::OrchardSnapshotProfile::Compact&&
            s.state.height==r.activation&&s.state.block_hash==r.base&&s.base_catalog.block==r.base&&
            s.parent_catalog.block==r.parent&&s.parent_catalog.height+1==r.activation&&
            s.parent_catalog.network==r.network&&s.parent_catalog.genesis==r.genesis&&
            s.parent_catalog.branch==r.branch&&s.parent_catalog.activation==r.activation&&
            s.parent_catalog.leaf_activation==r.leaf_activation);
    }
    struct Reader {
        std::span<const uint8_t> b;size_t at=0;
        explicit Reader(std::span<const uint8_t> bytes):b(bytes){}
        uint64_t Number(unsigned n){Check(at<=b.size()&&n<=b.size()-at);uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[at++])<<(8*i);return v;}
        uint256 Hash(){Check(at<=b.size()&&32<=b.size()-at);uint256 h;std::copy_n(b.begin()+at,32,h.begin());at+=32;return h;}
        void Tag(const char* tag,uint8_t version){Check(b.size()>=6&&std::memcmp(b.data(),tag,5)==0&&b[5]==version);at=6;}
        void End()const {Check(at==b.size());}
    };
};
} // namespace dinero::p2p
