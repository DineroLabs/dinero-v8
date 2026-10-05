#pragma once
#include "daemon/services/replay_header_history.h"
#include "consensus/pow.hpp"
#include "consensus/pow.h"
#include <deque>
#ifndef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet::detail {
struct RuntimeReplaySpoolTestAccess {
    static sqlite3* Handle(RuntimeReplaySpool& spool) { return spool.db_.get(); }
};
}
#endif
namespace dinero::assumeutxo {
struct ReplayHeaderHistoryTestAccess {
    static void Append(ReplayHeaderHistory& owner, const BlockHeader& header, uint32_t height) {
        owner.AppendValidated(header,height);
    }
    static sqlite3* Handle(ReplayHeaderHistory& owner) {
        return wallet::detail::RuntimeReplaySpoolTestAccess::Handle(owner.spool_);
    }
    static ReplayHeaderHistory& History(AssumeUtxoReplayEngine& engine) { return engine.headers_; }
    static uint32_t Height(const ReplayHeaderHistory& owner) { return owner.height_; }
    static uint256 Hash(const ReplayHeaderHistory& owner) { return owner.hash_; }
};
}
namespace dinero::consensus {
bool OriginalReplayHeaderRule(
    const BlockHeader& header,
    const HeaderIndexEntry* prev
) {
    // Phase N.1: Stateless header validation only

    // 1. Version sanity
    if (header.version < 1) {
        return false;
    }

    // 2. Timestamp rules
    if (header.timestamp == 0) {
        return false;
    }

    // For non-genesis blocks, enforce the same header-level time rule the
    // active chain accepts: a timestamp must be greater than median-time-past.
    // It does not have to be monotonic relative to the direct parent.
    if (prev != nullptr) {
        if (header.timestamp <= prev->GetMedianTimePast()) {
            return false;
        }
    }

    // 3. Difficulty target validation (if not genesis)
    if (prev != nullptr) {
        // In production, this would validate difficulty adjustment
        // For now, just check bits field is non-zero
        if (header.difficulty == 0) {
            return false;
        }
    }

    // 4. Proof-of-work validity — the header hash must meet the claimed target.
    //
    // SECURITY (header-PoW verification): without this, a peer can submit headers
    // claiming arbitrarily hard difficulty bits (hence arbitrarily large
    // GetBlockProof() chainwork) backed by NO real work, win fork-choice in
    // UpdateBestHeader(), and steer block download toward a forged chain — a
    // zero-cost sync-stall / eclipse vector. Full blocks are still rejected at
    // connect (block_acceptor), so this is availability, not theft; this check
    // closes the header-level hole and restores the "chainwork is lower-bounded
    // by real work" invariant fork-choice relies on.
    //
    // Regtest policy mirrors block_acceptor's PATH A exactly: regtest blocks are
    // mined deterministically (nonce=0, instant) and skip PoW at connect, so we
    // must skip it here too — otherwise a header for a block the node *would*
    // accept could fail header validation (startup-replay divergence / regtest
    // breakage). On mainnet/testnet, enforce hash <= target.
    //
    // require_standard=FALSE is deliberate and load-bearing. The live block-connect
    // PoW gate (pow_consensus_engine: CheckProofOfWork(blockHash, bits)) verifies
    // hash <= target but does NOT call CheckDifficultyBits. The ASERT schedule
    // legitimately eased early-block difficulty below MAX_BITS (0x1d31ffce) — e.g.
    // mainnet block 1 has bits 0x1E00C7FF, an easier target than MAX_BITS — so
    // CheckDifficultyBits()/require_standard=true would REJECT real historical
    // headers (difficulty<1) that block-connect accepted, bricking header sync and
    // startup replay. require_standard=false keeps the real hash <= target check
    // (NOT a stub) while dropping the min-difficulty floor block-connect never
    // imposed. Soundness is preserved: hash <= target still lower-bounds chainwork
    // by real work, and the ASERT check below pins bits to the exact required value
    // (a stronger constraint than any floor).
    uint256 hash = header.GetHash();
    if (hash.IsNull()) {
        return false;
    }
    if (!Params().SkipProofOfWork()) {
        if (!CheckProofOfWork(header, /*require_standard=*/false)) {
            return false;
        }
    }

    // 4b. Expected difficulty (ASERT schedule) — defense-in-depth.
    //
    // Mirrors block_acceptor's bad-diffbits check at the header level, using the
    // SAME shared computation (GetNextWorkRequiredForCandidate) so header
    // acceptance and block connect can never drift on the difficulty rule. A
    // header whose claimed bits != the bits required by the ASERT schedule for
    // its height is rejected before its (claimed) chainwork is credited.
    //
    // Computed from THIS header's own parent (`prev`) — prev->GetMedianTimePast()
    // walks prev's own ancestry, so side branches validate against their own
    // anchor context, not the active tip.
    //
    // Gating: skip genesis (prev == nullptr; handled by the PoW check above) and
    // skip regtest (block_acceptor PATH A skips difficulty there too — same-rule).
    // expected == 0 means "uncomputable" (pre-ASERT height / missing context):
    // skip rather than reject, so honest persisted headers replay cleanly at
    // startup and block_acceptor remains the backstop. Compact bits are compared
    // for equality against the canonical encoding (never ordered numerically).
    if (prev != nullptr && !Params().SkipProofOfWork()) {
        const Consensus consensus = GetConsensusForCurrentNetwork();
        const uint32_t expected_bits = GetNextWorkRequiredForCandidate(
            static_cast<int32_t>(prev->height) + 1,
            static_cast<int64_t>(header.timestamp),
            consensus,
            /*parent_index=*/static_cast<const CBlockIndex*>(nullptr),
            /*parent_entry=*/prev,
            /*chain_db=*/static_cast<dinero::NoChainDb*>(nullptr));
        // The isolated qualification profile never credits unverifiable work.
        // Preserve historical replay behavior on existing networks.
        if (expected_bits == 0 && Params().regtest_enforce_pow) return false;
        if (expected_bits != 0 && header.difficulty != expected_bits) {
            std::cerr << "[HeaderChainSelector] ❌ bad-diffbits-header at height "
                      << (prev->height + 1) << ": header has "
                      << std::hex << header.difficulty << ", required "
                      << expected_bits << std::dec
                      << " (hash " << hash.GetHex().substr(0, 16) << "...)"
                      << std::endl;
            return false;
        }
    }

    // 5. Linkage - prev_hash must match parent (already checked in AddHeader)

    // ❌ NOT validated here:
    // - Merkle root (requires transactions)
    // - UTXO validity
    // - Transaction rules

    return true;
}

} // namespace dinero::consensus
namespace dinero {
namespace {
struct ReplayHeaderProfileRestore {
    ChainParams saved=Params();
    ~ReplayHeaderProfileRestore(){MutableParams()=saved;}
};
BlockHeader SolveReplayHeader(BlockHeader header, bool valid) {
    for(uint32_t nonce=0;nonce<4'000'000;++nonce) {
        header.nonce=nonce;
        if(consensus::CheckProofOfWork(header,false)==valid)return header;
    }
    throw std::runtime_error("replay header fixture nonce range exhausted");
}
using ReplayHeaders=assumeutxo::ReplayHeaderHistory;
using ReplayAccess=assumeutxo::ReplayHeaderHistoryTestAccess;
void HeaderSql(sqlite3* db,const char* sql) {
    if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
}
TEST(ReplayHeaderHistory, LegacyReferenceAndBothMedianWidths) {
    ReplayHeaderProfileRestore restore;SelectParams(Chain::REGTEST);
    ReplayHeaders owner;consensus::HeaderChainSelector selector;
    std::deque<consensus::HeaderIndexEntry> reference;
    auto header=SelectedGenesis().header;
    for(uint32_t height=0;height<32;++height) {
        if(height) {
            header.prev_block_hash=header.GetHash();header.timestamp+=120;++header.nonce;
            if(height==13)header.timestamp+=uint64_t(UINT32_MAX)+1;
        }
        const auto* parent=reference.empty()?nullptr:&reference.back();
        EXPECT_TRUE(consensus::OriginalReplayHeaderRule(header,parent));
        ASSERT_TRUE(owner.Validate(header));ASSERT_TRUE(selector.AddHeader(header));
        reference.emplace_back(header,parent);ReplayAccess::Append(owner,header,height);
        for(uint32_t wanted=0;wanted<=height;++wanted) {
            std::vector<uint64_t> times;
            for(uint32_t h=wanted;;--h) {
                times.push_back(uint64_t(reference[h].header.timestamp));
                if(times.size()==11||h==0)break;
            }
            std::sort(times.begin(),times.end());
            const auto mtp=owner.LockMedianTimePast(header.GetHash(),wanted);
            ASSERT_TRUE(mtp);EXPECT_EQ(*mtp,times[times.size()/2]);
        }
        if(height) {
            auto bad=header;bad.prev_block_hash=header.GetHash();
            bad.timestamp=reference.back().GetMedianTimePast();
            EXPECT_FALSE(consensus::OriginalReplayHeaderRule(bad,&reference.back()));
            EXPECT_FALSE(owner.Validate(bad));
        }
    }
    ASSERT_GT(*owner.LockMedianTimePast(header.GetHash(),31),uint64_t(UINT32_MAX));
    EXPECT_FALSE(owner.LockMedianTimePast(uint256{},0));
    EXPECT_FALSE(owner.LockMedianTimePast(header.GetHash(),32));
}
TEST(ReplayHeaderHistory, HistoricalWorkAndTimingRulesRemainExact) {
    ReplayHeaderProfileRestore restore;SelectParams(Chain::REGTEST);
    MutableParams().regtest_enforce_pow=true;MutableParams().sixty_second_activation_height=4;
    ReplayHeaders owner;std::deque<consensus::HeaderIndexEntry> reference;
    auto header=SelectedGenesis().header;
    ASSERT_TRUE(consensus::OriginalReplayHeaderRule(header,nullptr));
    ASSERT_TRUE(owner.Validate(header));ReplayAccess::Append(owner,header,0);reference.emplace_back(header,nullptr);
    const auto params=GetConsensusForCurrentNetwork();
    for(uint32_t height=1;height<=7;++height) {
        auto child=header;child.prev_block_hash=header.GetHash();
        child.timestamp+=height<4?120:60;
        child.difficulty=GetNextWorkRequiredForCandidate(height,child.timestamp,params,
            static_cast<const CBlockIndex*>(nullptr),&reference.back(),static_cast<NoChainDb*>(nullptr));
        ASSERT_NE(child.difficulty,0u);child=SolveReplayHeader(child,true);
        for(int field=0;field<4;++field) {
            auto bad=child;
            if(field==0)bad.version=0;
            if(field==1)bad.timestamp=reference.back().GetMedianTimePast();
            if(field==2)bad.difficulty=0;
            if(field==3)bad=SolveReplayHeader(bad,false);
            EXPECT_FALSE(consensus::OriginalReplayHeaderRule(bad,&reference.back()));
            EXPECT_FALSE(owner.Validate(bad));
        }
        auto wrong_bits=child;wrong_bits.difficulty=child.difficulty==0x207fffff?0x207ffffe:0x207fffff;
        wrong_bits=SolveReplayHeader(wrong_bits,true);
        EXPECT_FALSE(consensus::OriginalReplayHeaderRule(wrong_bits,&reference.back()));
        EXPECT_FALSE(owner.Validate(wrong_bits));
        ASSERT_TRUE(consensus::OriginalReplayHeaderRule(child,&reference.back()));
        ASSERT_TRUE(owner.Validate(child));ReplayAccess::Append(owner,child,height);
        reference.emplace_back(child,&reference.back());header=child;
    }
}
TEST(ReplayHeaderHistory, FailedBodyPreservesHeaderPositionAndAllowsRetry) {
    const auto chain=BuildDeterministicChain(2);ASSERT_EQ(chain.size(),2u);
    assumeutxo::AssumeUtxoReplayEngine engine;std::string error;
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(),error));
    auto& history=ReplayAccess::History(engine);
    const auto hash=ReplayAccess::Hash(history);const auto before=engine.RecordsDigestHex();
    auto wrong=chain[0];
    wrong.vtx[0].vout[0].value=AmountUna::Una(wrong.vtx[0].vout[0].value.GetUna()-1);
    wrong.header.merkle_root=consensus::ComputeMerkleRoot(wrong.vtx);
    ASSERT_TRUE(history.Validate(wrong.header));
    EXPECT_FALSE(engine.ConnectAndAdvance(wrong,1,wrong.GetHash(),error));
    EXPECT_NE(error.find("bad-utreexo-root"),std::string::npos)<<error;
    EXPECT_EQ(engine.Height(),0u);EXPECT_EQ(engine.RecordsDigestHex(),before);
    EXPECT_EQ(ReplayAccess::Height(history),0u);EXPECT_EQ(ReplayAccess::Hash(history),hash);
    ASSERT_TRUE(engine.ConnectAndAdvance(chain[0],1,chain[0].GetHash(),error))<<error;
    ASSERT_TRUE(engine.ConnectAndAdvance(chain[1],2,chain[1].GetHash(),error))<<error;
    EXPECT_EQ(ReplayAccess::Height(history),2u);EXPECT_EQ(ReplayAccess::Hash(history),chain[1].GetHash());
}
TEST(ReplayHeaderHistory, StorageFailureRetiresActualReplayState) {
    const auto chain=BuildDeterministicChain(1);ASSERT_EQ(chain.size(),1u);
    for(bool commit_failure:{false,true}) {
        assumeutxo::AssumeUtxoReplayEngine engine;std::string error;
        ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(),error));
        auto& history=ReplayAccess::History(engine);auto* db=ReplayAccess::Handle(history);
        const auto previous=ReplayAccess::Hash(history);
        struct CommitProbe {ReplayHeaders* history;uint256 hash;bool saw=false,old=false;} probe{&history,previous};
        if(commit_failure)sqlite3_commit_hook(db,[](void* raw){
            auto& p=*static_cast<CommitProbe*>(raw);p.saw=true;
            p.old=ReplayAccess::Height(*p.history)==0&&ReplayAccess::Hash(*p.history)==p.hash;
            return 1;
        },&probe);
        else sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
            return action==SQLITE_INSERT&&table&&std::string(table)=="records"?SQLITE_DENY:SQLITE_OK;
        },nullptr);
        EXPECT_FALSE(engine.ConnectAndAdvance(chain[0],1,chain[0].GetHash(),error));
        EXPECT_EQ(error,"replay header storage unavailable");
        if(commit_failure){EXPECT_TRUE(probe.saw);EXPECT_TRUE(probe.old);}
        EXPECT_EQ(ReplayAccess::Height(history),0u);EXPECT_EQ(ReplayAccess::Hash(history),previous);
        sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);
        EXPECT_THROW(engine.Height(),std::runtime_error);
        EXPECT_THROW(engine.RecordsDigestHex(),std::runtime_error);
        EXPECT_THROW(engine.ProvenUtxos(),std::runtime_error);
        EXPECT_THROW(engine.Forest(),std::runtime_error);
        EXPECT_THROW(engine.ShieldedTree(),std::runtime_error);
        EXPECT_THROW(engine.ShieldedNullifiers(),std::runtime_error);
        EXPECT_THROW(engine.ShieldedAnchors(),std::runtime_error);
        EXPECT_THROW(engine.UndoTail(),std::runtime_error);
        EXPECT_FALSE(engine.ConnectAndAdvance(chain[0],1,chain[0].GetHash(),error));
        EXPECT_FALSE(engine.SeedGenesis(SelectedGenesis(),error));
    }
}
TEST(ReplayHeaderHistory, AlteredRecordsAndIncompleteReadsRefuse) {
    const auto chain=BuildDeterministicChain(4);ASSERT_EQ(chain.size(),4u);
    for(int failure=0;failure<3;++failure) {
        assumeutxo::AssumeUtxoReplayEngine engine;std::string error;
        ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(),error));
        for(uint32_t h=1;h<=3;++h)ASSERT_TRUE(engine.ConnectAndAdvance(chain[h-1],h,chain[h-1].GetHash(),error));
        auto& history=ReplayAccess::History(engine);auto* db=ReplayAccess::Handle(history);
        if(failure==0)HeaderSql(db,"UPDATE records SET v=zeroblob(128) WHERE k=X'6801000000'");
        if(failure==1)sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
            return action==SQLITE_READ&&table&&std::string(table)=="records"?SQLITE_DENY:SQLITE_OK;
        },nullptr);
        struct Interrupt {sqlite3* db;bool hit=false;} interrupt{db};
        if(failure==2)sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned,void* raw,void*,void*){
            auto& i=*static_cast<Interrupt*>(raw);if(!i.hit){i.hit=true;sqlite3_interrupt(i.db);}return 0;
        },&interrupt);
        EXPECT_FALSE(engine.ConnectAndAdvance(chain[3],4,chain[3].GetHash(),error));
        EXPECT_EQ(error,"replay header ancestry unavailable");
        if(failure==2)EXPECT_TRUE(interrupt.hit);
        sqlite3_trace_v2(db,0,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);
        EXPECT_EQ(ReplayAccess::Height(history),3u);
        EXPECT_THROW(engine.RecordsDigestHex(),std::runtime_error);
        EXPECT_FALSE(engine.ConnectAndAdvance(chain[3],4,chain[3].GetHash(),error));
    }
}
TEST(ReplayHeaderHistory, PagerSpillsAndOldTimeWindowsRemainAvailable) {
    ReplayHeaderProfileRestore restore;SelectParams(Chain::REGTEST);
    ReplayHeaders owner;auto header=SelectedGenesis().header;
    const uint64_t genesis_time=header.timestamp;
    for(uint32_t height=0;height<20000;++height) {
        if(height){header.prev_block_hash=header.GetHash();header.timestamp+=120;++header.nonce;}
        ASSERT_TRUE(owner.Validate(header));ReplayAccess::Append(owner,header,height);
    }
    const auto usage=owner.UsageNow();EXPECT_GT(usage.pager_bytes,0);
    EXPECT_LT(usage.pager_bytes,2*1024*1024);
    EXPECT_EQ(owner.LockMedianTimePast(header.GetHash(),0),std::optional<uint64_t>{genesis_time});
    EXPECT_EQ(owner.LockMedianTimePast(header.GetHash(),100),std::optional<uint64_t>{genesis_time+95*120});
    EXPECT_EQ(owner.LockMedianTimePast(header.GetHash(),19999),std::optional<uint64_t>{genesis_time+19994*120});
}
} // namespace dinero
