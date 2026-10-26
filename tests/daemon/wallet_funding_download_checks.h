#pragma once
#include "p2p/wallet_funding_download.h"
namespace dinero {
TEST(WalletFundingDownload, NewInputProofCodecBounds){
    using W=p2p::WalletInputProof;
    EXPECT_THROW(W::Request::Decode(std::vector<uint8_t>(W::RequestBytes)),std::invalid_argument);
    EXPECT_THROW(W::Reply::Decode(std::vector<uint8_t>(W::MaximumBytes+1)),std::invalid_argument);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(WalletFundingDownload, NewInputRequestCapacityAndStop){
    OrchardAdmissionFixture f;using W=p2p::WalletInputProof;W::Request request;
    request.magic=Params().magic;request.genesis=uint256::FromHexUnsafe(Params().genesis_hash);
    request.branch=Params().orchard_branch_id;request.activation=Params().orchard_activation_height;
    request.leaf_activation=consensus::GetUtreexoMaturityLeafActivationHeight();request.height=request.activation;
    request.tip=W::KeyNonce();request.root=W::KeyNonce();request.point=OutPoint(TxId(W::KeyNonce()),0);
    p2p::WalletInputProofDownload download([]{return std::vector<p2p::OrchardCatalogClient::Peer>{};},
        [](const auto&,const auto&,const auto&){return false;});
    for(size_t n=0;n<p2p::WalletInputProofDownload::MaximumItems;++n){request.point.vout=uint32_t(n);EXPECT_FALSE(download.Request(request));}
    request.point.vout=100;EXPECT_THROW(download.Request(request),std::runtime_error);
    download.Stop();EXPECT_FALSE(download.Request(request));
}
TEST(WalletFundingDownload, CurrentSourceChangeCancelsBlockedInputFlight){
    OrchardAdmissionFixture f;using W=p2p::WalletInputProof;W::Request request;
    request.magic=Params().magic;request.genesis=uint256::FromHexUnsafe(Params().genesis_hash);
    request.branch=Params().orchard_branch_id;request.activation=Params().orchard_activation_height;
    request.leaf_activation=consensus::GetUtreexoMaturityLeafActivationHeight();request.height=request.activation;
    request.tip=W::KeyNonce();request.root=W::KeyNonce();request.point=OutPoint(TxId(W::KeyNonce()),0);
    std::mutex mutex;std::vector<W::Request> sent;
    const p2p::OrchardCatalogClient::Peer peer{"blocked-input-peer",21,ServiceFlags::NODE_NETWORK};
    p2p::WalletInputProofDownload download([&]{return std::vector{peer};},[&](const auto&,const auto& command,const auto& bytes){
        if(command!=W::RequestCommand)return false;std::lock_guard lock(mutex);sent.push_back(W::Request::Decode(bytes));return true;});
    EXPECT_FALSE(download.Request(request));
    const auto count=[&]{std::lock_guard lock(mutex);return sent.size();};
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(count()<1&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(count(),1u);auto changed=request;changed.tip.begin()[1]=3;++changed.height;
    EXPECT_FALSE(download.Request(changed));until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(count()<2&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    download.Stop();ASSERT_EQ(count(),2u);EXPECT_EQ(sent[0].tip,request.tip);EXPECT_EQ(sent[1].tip,changed.tip);
    EXPECT_NE(sent[0].nonce,sent[1].nonce);
}
#endif
TEST(WalletFundingDownload, BoundedRequestsAndStoppedOwner){
    p2p::WalletFundingDownload download([]{return std::vector<p2p::WalletFundingDownload::Peer>{};},
        [](const auto&,const auto&,const auto&){return false;});
    EXPECT_THROW(download.Request(uint256{}),std::invalid_argument);
    for(unsigned i=1;i<=p2p::WalletFundingDownload::MaximumBodies;++i){uint256 id;id.begin()[0]=i;EXPECT_FALSE(download.Request(id));}
    uint256 extra;extra.begin()[0]=100;EXPECT_THROW(download.Request(extra),std::runtime_error);
    download.Stop();EXPECT_FALSE(download.Request(extra));
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(WalletFundingDownload, CurrentFullSourceInputProofAndExactSessionDownload){
    CanonicalRecoveryFixture f;f.MineEmpty();using W=p2p::WalletInputProof;
    const auto current=f.f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(current.ok());
    W::Request request;request.magic=Params().magic;request.genesis=uint256::FromHexUnsafe(Params().genesis_hash);
    request.branch=Params().orchard_branch_id;request.activation=Params().orchard_activation_height;
    request.leaf_activation=consensus::GetUtreexoMaturityLeafActivationHeight();request.height=current->height;
    request.tip=current->block_hash;std::copy(current->commitment.begin(),current->commitment.end(),request.root.begin());
    request.nonce=W::KeyNonce();request.point=OutPoint(f.f.blocks.at(2).vtx.front().GetTxid(),0);
    const auto proof=ServeWalletInputProof(*f.f.service,request);ASSERT_TRUE(proof);
    ASSERT_EQ(request.Encode().size(),W::RequestBytes);ASSERT_EQ(W::Reply::Decode(proof->Encode()).Encode(),proof->Encode());
    const auto* live=f.f.service->GetConsensusUTXOSet();ASSERT_NE(live,nullptr);
    const auto stump=[&]{const auto held=live->LockForestShared();return consensus::UtreexoStump::fromForest(live->GetForest());}();
    EXPECT_TRUE(proof->VerifyPath(stump));auto wrong=*proof;++wrong.value;EXPECT_FALSE(wrong.VerifyPath(stump));
    auto trailing=proof->Encode();trailing.push_back(0);EXPECT_THROW(W::Reply::Decode(trailing),std::invalid_argument);
    auto stale=request;stale.tip.begin()[0]^=1;EXPECT_FALSE(ServeWalletInputProof(*f.f.service,stale));
    p2p::WalletInputProofDownload* client=nullptr;std::atomic<bool> wrong_session=false,exact=false;
    const p2p::OrchardCatalogClient::Peer peer{"exact-input-peer",19,ServiceFlags::NODE_NETWORK};
    p2p::WalletInputProofServer server([&](const auto& q){return ServeWalletInputProof(*f.f.service,q);},
        [&](const auto& p,const auto& command,const auto& bytes){if(command!=W::ReplyCommand)return false;
            auto old=p;++old.session;wrong_session=client->Enqueue(old,bytes);exact=client->Enqueue(p,bytes);return bool(exact);});
    p2p::WalletInputProofDownload download([&]{return std::vector{peer};},[&](const auto& p,const auto& command,const auto& bytes){
        return command==W::RequestCommand&&server.Enqueue(p,bytes);});client=&download;
    EXPECT_FALSE(download.Request(request));std::shared_ptr<const W::Reply> ready;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!(ready=download.Request(request))&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    server.Stop();download.Stop();ASSERT_TRUE(ready);EXPECT_TRUE(ready->VerifyPath(stump));EXPECT_TRUE(exact);EXPECT_FALSE(wrong_session);
    EXPECT_FALSE(download.Request(request));
}
struct ImportedFundingCreationTestAccess {
    static bool Check(const Block& body,const BlockHeader& header,uint32_t height,const OutPoint& point,const consensus::UTXOEntry& coin){
        return ChainstateService::CheckImportedFundingCreation(body,header,height,point,coin);
    }
};
TEST(WalletFundingDownload, ActualCreationMetadataAndMerkleComparison){
    OrchardAdmissionFixture f;f.preserve_evidence=true;const auto& body=f.blocks.at(1);const auto& tx=body.vtx.front();
    const OutPoint point(tx.GetTxid(),0);
    const consensus::UTXOEntry coin(tx.vout.front().value,tx.vout.front().scriptPubKey,1,true);
    const auto check=[&](const auto& b,const auto& h,uint32_t height,const auto& p,const auto& c){
        return ImportedFundingCreationTestAccess::Check(b,h,height,p,c);};
    ASSERT_TRUE(check(body,body.header,1,point,coin));
    auto wrong=coin;wrong.height=2;EXPECT_FALSE(check(body,body.header,1,point,wrong));
    wrong=coin;wrong.isCoinbase=false;EXPECT_FALSE(check(body,body.header,1,point,wrong));
    wrong=coin;wrong.value=AmountUna::Una(1);EXPECT_FALSE(check(body,body.header,1,point,wrong));
    wrong=coin;wrong.scriptPubKey.push_back(0);EXPECT_FALSE(check(body,body.header,1,point,wrong));
    wrong=coin;wrong.is_confidential=true;EXPECT_FALSE(check(body,body.header,1,point,wrong));
    auto missing=point;++missing.vout;EXPECT_FALSE(check(body,body.header,1,missing,coin));
    auto changed=body;changed.vtx.front().vout.front().scriptPubKey.push_back(0);
    EXPECT_FALSE(check(changed,body.header,1,point,coin));
    changed=body;changed.vtx.push_back(changed.vtx.front());EXPECT_FALSE(check(changed,body.header,1,point,coin));
    // Pure actual-body comparison only, not a selected source/coin capability.
}
TEST(WalletFundingDownload, ExactSessionAndBodyDataOnly){
    OrchardAdmissionFixture f;
    const auto raw=f.blocks.at(101).Serialize();const std::vector<uint8_t> bytes(raw.begin(),raw.end());
    const auto hash=f.blocks.at(101).GetHash();
    p2p::WalletFundingDownload* actor=nullptr;std::atomic<bool> sent=false,wrong_session=false,exact=false;
    const p2p::WalletFundingDownload::Peer peer{"fixture-peer",17,ServiceFlags::NODE_NETWORK};
    p2p::WalletFundingDownload download([&]{return std::vector{peer};},[&](const auto& selected,const auto& command,const auto& request){
        if(command!="getdata"||request.size()!=37||!std::equal(hash.begin(),hash.end(),request.begin()+5))return false;
        auto stale=selected;++stale.session;wrong_session=actor->Enqueue(stale,bytes);
        exact=actor->Enqueue(selected,bytes);sent=true;return true;
    });actor=&download;
    EXPECT_FALSE(download.Request(hash));
    std::shared_ptr<const std::vector<uint8_t>> ready;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!(ready=download.Request(hash))&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    download.Stop();ASSERT_TRUE(ready);EXPECT_EQ(*ready,bytes);EXPECT_TRUE(sent);EXPECT_FALSE(wrong_session);EXPECT_TRUE(exact);
    // No owner, wallet, DB, history receipt or execution capability is returned.
    EXPECT_FALSE(download.Request(hash));
}
#endif
}
