#pragma once
// Included inside namespace dinero, after the normal consensus/header includes.
// Shared by the offline vector generator and the runtime capacity fixture.
namespace retention_vectors {
struct Profile {
    ChainParams previous=Params();
    Profile() {
        if(Params().name!="regtest")throw std::runtime_error("retention vectors require regtest");
        MutableParams().regtest_enforce_pow=true;
        MutableParams().sixty_second_activation_height=102;
        MutableParams().orchard_activation_height=102;
        MutableParams().orchard_branch_id=1;
    }
    ~Profile(){MutableParams()=previous;}
    Profile(const Profile&)=delete;
    Profile& operator=(const Profile&)=delete;
};
inline BlockHeader Child(const consensus::HeaderIndexEntry& parent,uint64_t salt,uint32_t nonce) {
    auto h=parent.header;h.prev_block_hash=parent.hash;h.timestamp+=120;
    h.merkle_root=uint256{};
    for(size_t i=0;i<sizeof(salt);++i)h.merkle_root.begin()[i]=uint8_t(salt>>(8*i));
    h.merkle_root.begin()[31]=0xa7;
    h.difficulty=GetNextWorkRequiredForCandidate(parent.height+1,h.timestamp,
        GetConsensusForCurrentNetwork(),static_cast<const CBlockIndex*>(nullptr),
        &parent,static_cast<NoChainDb*>(nullptr));
    if(!h.difficulty)throw std::runtime_error("retention vector difficulty unavailable");
    h.nonce=nonce;return h;
}
template<class Solve> std::deque<consensus::HeaderIndexEntry> History(Solve&& solve) {
    std::deque<consensus::HeaderIndexEntry> history;
    history.emplace_back(BuildCanonicalGenesis(Params()).header,nullptr);
    for(uint64_t h=1;h<=103;++h) {
        auto header=solve(history.back(),900000+h);
        history.emplace_back(header,&history.back());
    }
    return history;
}
} // namespace retention_vectors
