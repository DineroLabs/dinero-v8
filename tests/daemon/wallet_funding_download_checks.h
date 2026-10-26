#pragma once
#include "p2p/wallet_funding_download.h"
namespace dinero {
TEST(WalletFundingDownload, BoundedRequestsAndStoppedOwner){
    p2p::WalletFundingDownload download([]{return std::vector<p2p::WalletFundingDownload::Peer>{};},
        [](const auto&,const auto&,const auto&){return false;});
    EXPECT_THROW(download.Request(uint256{}),std::invalid_argument);
    for(unsigned i=1;i<=p2p::WalletFundingDownload::MaximumBodies;++i){uint256 id;id.begin()[0]=i;EXPECT_FALSE(download.Request(id));}
    uint256 extra;extra.begin()[0]=100;EXPECT_THROW(download.Request(extra),std::runtime_error);
    download.Stop();EXPECT_FALSE(download.Request(extra));
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
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
