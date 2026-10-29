#include "consensus/orchard_snapshot.h"
#include "consensus/tx_validation.h"
#include "consensus/orchard_state_root.h"
#include "consensus/orchard_catalog_transition.h"
#include "consensus/orchard_forest_transition.h"
#include "consensus/orchard_profile.h"
#include "consensus/orchard_block_filter.h"
#include "consensus/pow.h"
#include "consensus/witness_commitment.h"
#include "consensus/merkle_root.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include <gtest/gtest.h>
#include <bit>
#include <set>
#include <sstream>
#include <fstream>
#include "crypto/secure_random.h"
#include "util/hex.h"
#include <sqlite3.h>

namespace {
using namespace dinero;
using namespace dinero::consensus;
namespace c=storage::catalog;
uint256 H(uint8_t n) {uint256 h;h.data[0]=n;return h;}
struct Fixture {
    OrchardSnapshotSection section;
    std::map<uint256,std::string,OrchardSnapshotRawLess> all;
    Fixture() {
        auto& s=section;s.state={150000,H(2),H(40),3,0,"opaque-frontier"};
        s.retirement={{2,H(90),17,150000,3,H(1),H(55),37,H(60),5,5},150000,H(2),H(1)};
        s.anchors={{H(40),1}};
        auto& parent=s.parent_catalog;parent.network=2;parent.genesis=H(90);parent.branch=17;
        parent.activation=150000;parent.leaf_activation=120000;parent.height=149999;parent.block=H(1);
        parent.parent=H(8);parent.work=arith_uint256{10};parent.stump=UtreexoStump{}.serialize();
        c::Tree tx(c::Kind::Transactions,Read(),Write());
        for(uint8_t n:{20,40,60}){parent.transactions=tx.Insert(parent.transactions,c::TransactionKey(TxId(H(n))),{});++parent.transaction_count;}
        auto& base=s.base_catalog;base=parent;base.height=150000;base.block=H(2);base.parent=H(1);
        base.work=arith_uint256{11};base.previous_record=c::Hash(parent.Encode());base.undo=H(70);
        base.transactions=tx.Insert(base.transactions,c::TransactionKey(TxId(H(80))),{});++base.transaction_count;
        s.base_body={1,2,3};s.accumulator={4,5,6};
        CaptureReachable();
    }
    c::Tree::Read Read() {return [this](const uint256& h){return all.at(h);};}
    c::Tree::Write Write() {return [this](const uint256& h,const std::string& b){all.emplace(h,b);};}
    void CaptureReachable() {
        section.nodes.clear();
        for(const auto* state:{&section.parent_catalog,&section.base_catalog})
            c::Tree(c::Kind::Transactions,Read()).ForEachNode(state->transactions,state->transaction_count,
                [&](const auto& id,const auto& bytes){section.nodes.emplace(id,bytes);return true;});
    }
};
TEST(OrchardSnapshotSection, FullAndCompactRoundTripCanonicalBytes) {
    for(const auto profile:{OrchardSnapshotProfile::Full,OrchardSnapshotProfile::Compact}) {
        Fixture f;auto& s=f.section;s.profile=profile;
        if(profile==OrchardSnapshotProfile::Compact)s.accumulator=s.base_catalog.stump;
        const auto encoded=EncodeOrchardSnapshotSection(s);const auto decoded=DecodeOrchardSnapshotSection(encoded);
        EXPECT_EQ(EncodeOrchardSnapshotSection(decoded),encoded);
        EXPECT_EQ(decoded.profile,profile);EXPECT_EQ(decoded.state,s.state);EXPECT_EQ(decoded.retirement,s.retirement);
        EXPECT_EQ(decoded.anchors,s.anchors);EXPECT_EQ(decoded.nodes,s.nodes);
        EXPECT_EQ(decoded.base_body,s.base_body);EXPECT_EQ(decoded.accumulator,s.accumulator);
    }
}
TEST(OrchardSnapshotSection, StreamingCodecIsByteIdenticalAndFailsClosed) {
    for(const auto profile:{OrchardSnapshotProfile::Full,OrchardSnapshotProfile::Compact}) {
        Fixture f;f.section.profile=profile;
        if(profile==OrchardSnapshotProfile::Compact)f.section.accumulator=f.section.base_catalog.stump;
        const auto encoded=EncodeOrchardSnapshotSection(f.section);std::vector<uint8_t> streamed;
        EXPECT_EQ(OrchardSnapshotSectionEncodedSize(f.section),encoded.size());
        EXPECT_EQ(StreamOrchardSnapshotSection(f.section,[&](auto bytes){streamed.insert(streamed.end(),bytes.begin(),bytes.end());}),encoded.size());
        EXPECT_EQ(streamed,encoded);
        EXPECT_EQ(HashOrchardSnapshotSection(f.section),storage::catalog::Hash(std::string(encoded.begin(),encoded.end())));
        for(size_t size=0;size<=encoded.size();++size) {
            size_t offset=0;
            const auto source=[&](std::span<uint8_t> bytes) {
                if(offset>size||bytes.size()>size-offset)throw std::runtime_error("short source");
                std::copy_n(encoded.begin()+offset,bytes.size(),bytes.begin());offset+=bytes.size();
            };
            if(size==encoded.size()) {
                const auto decoded=ReadOrchardSnapshotSection(size,source);
                EXPECT_EQ(EncodeOrchardSnapshotSection(decoded),encoded);EXPECT_EQ(offset,size);
            } else EXPECT_ANY_THROW((void)ReadOrchardSnapshotSection(size,source))<<size;
        }
        EXPECT_ANY_THROW(StreamOrchardSnapshotSection(f.section,[](auto){throw std::runtime_error("short sink");}));
        EXPECT_ANY_THROW((void)ReadOrchardSnapshotSection(ORCHARD_SNAPSHOT_MAX_SECTION_BYTES+1,[](auto){FAIL()<<"must not read";}));
    }
}
TEST(OrchardSnapshotSection, TruncationTrailingBytesAndUnknownProfilesRefuse) {
    Fixture f;const auto bytes=EncodeOrchardSnapshotSection(f.section);
    for(size_t size=0;size<bytes.size();++size)
        EXPECT_ANY_THROW((void)DecodeOrchardSnapshotSection(std::span(bytes).first(size)))<<size;
    auto changed=bytes;changed.push_back(0);EXPECT_ANY_THROW((void)DecodeOrchardSnapshotSection(changed));
    for(uint8_t profile:{0,3,255}){changed=bytes;changed[8]=profile;EXPECT_ANY_THROW((void)DecodeOrchardSnapshotSection(changed));}
    changed=bytes;changed[4]=2;EXPECT_ANY_THROW((void)DecodeOrchardSnapshotSection(changed));
    changed=bytes;changed[0]^=1;EXPECT_ANY_THROW((void)DecodeOrchardSnapshotSection(changed));
}
TEST(OrchardSnapshotSection, LogicalMembershipUsesRawByteOrderAndExactReferenceCounts) {
    Fixture f;auto& s=f.section;
    s.nullifiers={H(1),H(2)};s.state.tree_size=2;
    const auto good=ComputeOrchardSnapshotSets(s);EXPECT_EQ(good.nullifier_count,2u);EXPECT_EQ(good.anchor_references,1u);
    auto changed=s;std::swap(changed.nullifiers[0],changed.nullifiers[1]);EXPECT_ANY_THROW((void)ComputeOrchardSnapshotSets(changed));
    changed=s;changed.nullifiers[1]=changed.nullifiers[0];EXPECT_ANY_THROW((void)ComputeOrchardSnapshotSets(changed));
    changed=s;changed.anchors.push_back(changed.anchors.front());EXPECT_ANY_THROW((void)ComputeOrchardSnapshotSets(changed));
    changed=s;changed.anchors.front().references=0;EXPECT_ANY_THROW((void)ComputeOrchardSnapshotSets(changed));
    changed=s;changed.anchors.front().references=2;EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(changed));
    changed=s;changed.nullifiers.pop_back();EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(changed));
    std::string bytes("ONF1\x01",5);bytes.append(reinterpret_cast<const char*>(H(1).data),32);
    bytes.append(reinterpret_cast<const char*>(H(2).data),32);c::Number(bytes,2,8);
    EXPECT_EQ(good.nullifiers,c::Hash(bytes));
    bytes=std::string("OAN1\x01",5);bytes.append(reinterpret_cast<const char*>(H(40).data),32);
    c::Number(bytes,1,8);c::Number(bytes,1,8);c::Number(bytes,1,8);
    EXPECT_EQ(good.anchors,c::Hash(bytes));
}
TEST(OrchardSnapshotSection, MissingOffPathNodeCorruptionAndUnreachableExtrasRefuse) {
    Fixture f;const auto good=f.section;
    for(const auto& [id,bytes]:good.nodes) {
        f.section=good;f.section.nodes.erase(id);EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(f.section));
        f.section=good;f.section.nodes[id].back()^=1;EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(f.section));
    }
    f.section=good;c::Node extra{c::Kind::Transactions};extra.key[0]=99;
    const auto bytes=extra.Encode();f.section.nodes.emplace(c::Hash(bytes),bytes);
    EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(f.section));
}
OrchardSnapshotSection DiskSection(OrchardSnapshotSection value,const std::filesystem::path& workspace={}) {
    auto inventory=std::make_shared<OrchardSnapshotCatalog>(workspace);
    for(const auto& [id,bytes]:value.nodes)inventory->InsertUnique(id,bytes);
    inventory->Seal();value.nodes.clear();value.disk_nodes=std::move(inventory);return value;
}
TEST(OrchardSnapshotDiskCatalog, ByteIdentitySharedOwnershipAndExactReachableUnion) {
    Fixture f;const auto bytes=EncodeOrchardSnapshotSection(f.section);
    auto disk=DiskSection(f.section);EXPECT_TRUE(disk.nodes.empty());
    EXPECT_EQ(OrchardSnapshotNodeCount(disk),f.section.nodes.size());EXPECT_EQ(EncodeOrchardSnapshotSection(disk),bytes);
    auto retained=disk;disk={};EXPECT_EQ(EncodeOrchardSnapshotSection(retained),bytes);
    auto wrong_count=retained;++wrong_count.base_catalog.transaction_count;
    EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(wrong_count));
    auto mixed=retained;mixed.nodes=f.section.nodes;EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(mixed));
    for(const auto& [id,payload]:f.section.nodes) {
        auto missing=f.section;missing.nodes.erase(id);
        EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(DiskSection(std::move(missing))));
    }
    auto extra=f.section;c::Node node{c::Kind::Transactions};node.key[0]=99;
    const auto payload=node.Encode();extra.nodes.emplace(c::Hash(payload),payload);
    EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(DiskSection(std::move(extra))));
    size_t offset=0;const auto streamed=ReadOrchardSnapshotSection(bytes.size(),[&](auto out){
        ASSERT_LE(out.size(),bytes.size()-offset);std::copy_n(bytes.begin()+offset,out.size(),out.begin());offset+=out.size();
    });
    EXPECT_TRUE(streamed.nodes.empty());ASSERT_TRUE(streamed.disk_nodes);EXPECT_EQ(EncodeOrchardSnapshotSection(streamed),bytes);
}
TEST(OrchardSnapshotDiskCatalog, PrivateWorkspaceCleanupAndCachedWalkStillCheckDiskIntegrity) {
    const auto path=std::filesystem::temp_directory_path()/("dinero-v6-disk-"+util::hex(secure_random_bytes(16)));
    ASSERT_TRUE(std::filesystem::create_directory(path));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{path};
    {std::ofstream user(path/"keep");user<<"caller data";}
    Fixture f;
    {
        auto disk=DiskSection(f.section,path);ASSERT_NO_THROW(ValidateOrchardSnapshotSection(disk));
        auto changed=disk;changed.base_catalog.transactions=f.section.nodes.begin()->first;
        EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(changed));
        std::filesystem::path database;
        for(const auto& child:std::filesystem::directory_iterator(path))if(child.is_directory())database=child.path()/"records.sqlite";
        ASSERT_FALSE(database.empty());sqlite3* raw=nullptr;
        ASSERT_EQ(sqlite3_open_v2(database.string().c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr),SQLITE_OK);
        struct Close{sqlite3* db;~Close(){sqlite3_close_v2(db);}} close{raw};
        ASSERT_EQ(sqlite3_exec(raw,"UPDATE records SET v=x'00'",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(disk));
        EXPECT_ANY_THROW(EncodeOrchardSnapshotSection(disk));
    }
    size_t children=0;for(const auto& child:std::filesystem::directory_iterator(path)){EXPECT_EQ(child.path().filename(),"keep");++children;}
    EXPECT_EQ(children,1u);
    EXPECT_ANY_THROW(DiskSection(f.section,path/"missing"));
    {std::ifstream user(path/"keep");EXPECT_EQ(std::string(std::istreambuf_iterator<char>(user),{}),"caller data");}
}
TEST(OrchardSnapshotSection, DomainParentRetirementAndCompactStumpMustAgree) {
    Fixture f;const auto good=f.section;
    for(int field=0;field<10;++field) {
        auto s=good;
        if(field==0)++s.retirement.record.branch_id;
        if(field==1)s.retirement.block_hash=H(99);
        if(field==2)s.retirement.parent_hash=H(99);
        if(field==3)++s.parent_catalog.height;
        if(field==4)s.base_catalog.previous_record=H(99);
        if(field==5)++s.base_catalog.leaf_activation;
        if(field==6)s.retirement.record.retired_value=MAX_MONEY;
        if(field==7)s.state.frontier.clear();
        if(field==8)s.base_catalog.transaction_count++;
        if(field==9){s.profile=OrchardSnapshotProfile::Compact;s.accumulator={0};}
        EXPECT_ANY_THROW(ValidateOrchardSnapshotSection(s))<<field;
    }
}
TEST(OrchardCatalogTraversal, WalkChecksMaximumKeyDepthCountsAndAllPaths) {
    std::map<uint256,std::string,OrchardSnapshotRawLess> nodes;
    c::Tree tree(c::Kind::Transactions,[&](const auto& id){return nodes.at(id);},
        [&](const auto& id,const auto& bytes){nodes.emplace(id,bytes);});
    uint256 root;c::Key key{};root=tree.Insert(root,key,{});
    for(unsigned bit=0;bit<256;++bit){key={};key[bit/8]=uint8_t(1u<<(7-bit%8));root=tree.Insert(root,key,{});}
    size_t visited=0;
    EXPECT_EQ(tree.ForEachNode(root,257,[&](const auto&,const auto&){++visited;return true;}),257u);
    EXPECT_EQ(visited,513u);
    EXPECT_ANY_THROW(tree.ForEachNode(root,256,[](const auto&,const auto&){return true;}));
    EXPECT_ANY_THROW(tree.ForEachNode(root,258,[](const auto&,const auto&){return true;}));
    EXPECT_ANY_THROW(tree.ForEachNode(root,257,[](const auto&,const auto&){return false;}));
    EXPECT_EQ(tree.ForEachNode({},0,[](const auto&,const auto&){return false;}),0u);
    EXPECT_ANY_THROW(tree.ForEachNode({},1,[](const auto&,const auto&){return true;}));
}

// Genuine codec, frontier, coin validation, catalog producer and compact proof
// transition over a synthetic coinbase-only block. Header selection/PoW is a
// caller precondition of this component; this fixture does not claim a network
// bootstrap or grant a durable compact owner.
struct AuthenticatedFixture {
    OrchardSnapshotCoins coins;
    ChainParams saved=Params();
    OrchardSnapshotSection section;
    BlockHeader parent{},header{};
    OrchardBlockContext context;
    arith_uint256 parent_work{10},work;
    UtreexoForest forest;
    OrchardBlockContext Context(const BlockHeader& h) {
        OrchardBlockContext value;value.height=22;value.activation_height=22;value.block_hash=h.GetHash();
        value.parent_hash=h.prev_block_hash;value.domain.network_code=2;value.domain.branch_id=Params().orchard_branch_id;
        const auto genesis=uint256::FromHexUnsafe(Params().genesis_hash);
        std::copy(genesis.begin(),genesis.end(),value.domain.genesis_wire.begin());return value;
    }
    AuthenticatedFixture() {
        SelectParams(Chain::REGTEST);
        MutableParams().orchard_activation_height=22;MutableParams().orchard_branch_id=0xa1b2c3d4;
        forest.setCanonicalEmptyRoots(true);
        const auto forest_root=forest.getCommitment();
        parent.version=1;parent.prev_block_hash=H(3);parent.timestamp=1700000000;parent.difficulty=0x207fffff;
        std::copy(forest_root.begin(),forest_root.end(),parent.utreexo_root.begin());
        header=parent;header.prev_block_hash=parent.GetHash();header.timestamp+=120;
        context=Context(header);
        auto& s=section;auto& catalog=s.parent_catalog;
        catalog.network=context.domain.network_code;std::copy(context.domain.genesis_wire.begin(),context.domain.genesis_wire.end(),catalog.genesis.begin());
        catalog.branch=context.domain.branch_id;catalog.activation=22;catalog.leaf_activation=GetUtreexoMaturityLeafActivationHeight();
        catalog.height=21;catalog.block=parent.GetHash();catalog.parent=parent.prev_block_hash;catalog.work=parent_work;
        catalog.stump=UtreexoStump::fromForest(forest).serialize();
        s.retirement.record={catalog.network,catalog.genesis,catalog.branch,22,3,parent.GetHash(),H(55),37,H(60),0,0};
        const OrchardStateLookups lookups{[](const auto&)->StatusOr<bool>{return false;},[](const auto&)->StatusOr<bool>{return false;}};
        const auto prepared=PrepareOrchardStateTransition(context,{}, {},lookups);
        s.state=prepared.Next();s.anchors={{s.state.anchor,1}};
        Transaction cb;cb.version=2;TxInput input;input.prevout.vout=UINT32_MAX;input.scriptSig={1,22,0};cb.vin={input};
        cb.vout.emplace_back(AmountUna::Una(1),std::vector<uint8_t>{0x51});
        const std::vector<WTxId> witness_ids{WTxId(uint256{})};
        cb.vout.emplace_back(AmountUna::Zero(),BuildWitnessCommitmentFromRoot(ComputeWitnessMerkleRootFromIds(witness_ids)));
        struct EmptyCoins final:ChainStateView {
            StatusOr<UTXOEntry> getCoin(const OutPoint&)const override{return Status::NotFound;}
            bool hasCoin(const OutPoint&)const override{return false;}
            uint32_t getHeight()const override{return 21;}
        } empty;
        const auto encode=[&](bool proof) {
            header.merkle_root=cb.GetTxid().AsUint256();const auto prefix=header.SerializeForHash();
            std::vector<uint8_t> bytes(prefix.begin(),prefix.end());bytes.push_back(1);
            const auto tx=cb.Serialize(TxSerializationMode::WithWitness);bytes.insert(bytes.end(),tx.begin(),tx.end());
            bytes.push_back(proof?1:0);
            if(proof) {
                BlockUtreexoData p;p.accumulator_root_before=forest.getCommitment();
                p.spend_proof=forest.generateBlockProof({},GetUtreexoProofFormatVersion(22));
                const auto wire=p.serialize();bytes.insert(bytes.end(),wire.begin(),wire.end());
            }
            return OrchardBlockCandidate::DecodeExact(bytes);
        };
        auto draft=encode(false);context=Context(draft.Header());
        const auto initial=PrepareOrchardBlockCoinsUnderChainstateLock(draft,context,empty,{},true);
        const auto filter=BuildOrchardBlockFilter(initial).GetHash();
        std::vector<uint8_t> script{0x6a,37,0x44,0x4e,0x52,0x46,1};script.insert(script.end(),filter.begin(),filter.end());
        cb.vout.emplace_back(AmountUna::Zero(),script);
        const auto root=ComputeOrchardCatalogStateRoot({context.domain,22,22,parent.GetHash()},
            s.retirement.record,s.state,ComputeOrchardSnapshotSets(s),catalog);
        cb.vout.emplace_back(AmountUna::Zero(),BuildStateCommitmentScript(root,StateCommitmentEncoding::Orchard));
        auto state_draft=encode(false);context=Context(state_draft.Header());
        const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(state_draft,context,empty,{},true);
        const auto transition=PrepareOrchardForestTransition(coins,parent,forest);header.utreexo_root=transition.Root();
        const auto block=encode(true);context=Context(block.Header());header=block.Header();
        const auto final_coins=PrepareOrchardBlockCoinsUnderChainstateLock(block,context,empty,{},true);
        s.state.block_hash=context.block_hash;s.retirement.height=22;s.retirement.block_hash=context.block_hash;s.retirement.parent_hash=parent.GetHash();
        work=parent_work+GetBlockProof(header.difficulty);
        std::map<uint256,std::string,OrchardSnapshotRawLess> all;
        s.base_catalog=AdvanceOrchardCatalog(catalog,final_coins,UtreexoStump::fromForest(transition.After()),work,
            BuildOrchardCoinUndo(final_coins).Serialize(),[&](const auto& id){return all.at(id);},
            [&](const auto& id,const auto& bytes){all.emplace(id,bytes);});
        s.base_body=block.WireBytes();s.accumulator=transition.After().serialize();
        for(const auto& change:final_coins.Changes())if(change.after)this->coins.emplace(change.outpoint,*change.after);
        const auto read=[&](const auto& id){return all.at(id);};
        for(const auto kind:{c::Kind::Transactions,c::Kind::LegacyCoins,c::Kind::NonTransparentCoins}) {
            const auto root=kind==c::Kind::Transactions?s.base_catalog.transactions:kind==c::Kind::LegacyCoins?s.base_catalog.legacy:s.base_catalog.nontransparent;
            const auto count=kind==c::Kind::Transactions?s.base_catalog.transaction_count:kind==c::Kind::LegacyCoins?s.base_catalog.legacy_count:s.base_catalog.nontransparent_count;
            c::Tree(kind,read).ForEachNode(root,count,[&](const auto& id,const auto& bytes){s.nodes.emplace(id,bytes);return true;});
        }
    }
    ~AuthenticatedFixture(){SelectParams(saved.name=="mainnet"?Chain::MAINNET:saved.name=="testnet"?Chain::TESTNET:Chain::REGTEST);MutableParams()=saved;}
    void Verify(const OrchardSnapshotSection& value,OrchardSnapshotProfile profile) {
        VerifyOrchardSnapshotSection(value,profile,context,parent,header,parent_work,work,context.block_hash,28,6,{},true);
    }
};
TEST(OrchardSnapshotAuthentication, RealFrontierBodyCatalogAndBothAccumulatorProfiles) {
    AuthenticatedFixture f;
    ASSERT_NO_THROW(f.Verify(f.section,OrchardSnapshotProfile::Full));
    auto compact=f.section;compact.profile=OrchardSnapshotProfile::Compact;compact.accumulator=compact.base_catalog.stump;
    ASSERT_NO_THROW(f.Verify(DecodeOrchardSnapshotSection(EncodeOrchardSnapshotSection(compact)),OrchardSnapshotProfile::Compact));
    EXPECT_ANY_THROW(f.Verify(compact,OrchardSnapshotProfile::Full));
    EXPECT_ANY_THROW(f.Verify(f.section,OrchardSnapshotProfile::Compact));
    for(int field=0;field<6;++field) {
        auto changed=f.section;
        if(field==0)changed.base_catalog.undo=H(99);
        if(field==1){changed.parent_catalog.leaf_activation++;changed.base_catalog.leaf_activation++;changed.base_catalog.previous_record=c::Hash(changed.parent_catalog.Encode());}
        if(field==2)changed.state.pool_balance++;
        if(field==3)changed.base_body.back()^=1;
        if(field==4)changed.accumulator.push_back(0);
        if(field==5)changed.retirement.record.retired_value++;
        // The attacker gets to recompute canonical transport framing. The
        // cryptographic/transition check still refuses these forged sections.
        const auto decoded=DecodeOrchardSnapshotSection(EncodeOrchardSnapshotSection(changed));
        EXPECT_ANY_THROW(f.Verify(decoded,OrchardSnapshotProfile::Full))<<field;
    }
    EXPECT_ANY_THROW(VerifyOrchardSnapshotSection(f.section,OrchardSnapshotProfile::Full,f.context,f.parent,f.header,
        f.parent_work,f.work,H(99),28,6,{},true));
    EXPECT_ANY_THROW(VerifyOrchardSnapshotSection(f.section,OrchardSnapshotProfile::Full,f.context,f.parent,f.header,
        f.parent_work,f.work,f.context.block_hash,27,6,{},true));
}
TEST(OrchardSnapshotFile, BothProfilesCanonicalChecksumsAndCoinCompleteness) {
    AuthenticatedFixture f;
    for(const auto profile:{OrchardSnapshotProfile::Full,OrchardSnapshotProfile::Compact}) {
        auto section=f.section;section.profile=profile;
        OrchardSnapshotCoins coins=f.coins;
        if(profile==OrchardSnapshotProfile::Compact){coins.clear();section.accumulator=section.base_catalog.stump;}
        SnapshotMetadata header;header.version=SNAPSHOT_VERSION_V6;header.block_hash=section.state.block_hash;
        header.block_height=section.state.height;header.utxo_count=coins.size();header.timestamp=f.header.timestamp;
        std::ostringstream stream(std::ios::binary);ASSERT_NO_THROW(WriteOrchardSnapshotFile(stream,header,section,coins));
        const auto bytes=stream.str();std::istringstream input(bytes,std::ios::binary);
        const auto parsed=ReadOrchardSnapshotFile(input,bytes.size(),profile);
        EXPECT_EQ(parsed.bytes,bytes.size());EXPECT_EQ(parsed.coins.size(),coins.size());
        ASSERT_NO_THROW(f.Verify(parsed.orchard,profile));
        std::ostringstream again(std::ios::binary);WriteOrchardSnapshotFile(again,parsed.metadata,parsed.orchard,parsed.coins);
        EXPECT_EQ(again.str(),bytes);
        for(size_t position=0;position<bytes.size();++position) {
            std::istringstream truncated(bytes.substr(0,position),std::ios::binary);
            EXPECT_ANY_THROW(ReadOrchardSnapshotFile(truncated,bytes.size(),profile))<<position;
        }
        for(const size_t position:{size_t(0),size_t(4),size_t(60),bytes.size()/2,bytes.size()-1}) {
            auto changed=bytes;changed[position]^=1;std::istringstream forged(changed,std::ios::binary);
            EXPECT_ANY_THROW(ReadOrchardSnapshotFile(forged,bytes.size(),profile));
        }
        std::istringstream trailing(bytes+"x",std::ios::binary);
        EXPECT_ANY_THROW(ReadOrchardSnapshotFile(trailing,bytes.size()+1,profile));
        std::istringstream capped(bytes,std::ios::binary);
        EXPECT_ANY_THROW(ReadOrchardSnapshotFile(capped,bytes.size()-1,profile));
    }
    ASSERT_NO_THROW(VerifyOrchardSnapshotCoins(f.section,f.coins));
    EXPECT_ANY_THROW(VerifyOrchardSnapshotCoins(f.section,{}));
    for(int field=0;field<6;++field) {
        auto coins=f.coins;auto& coin=coins.begin()->second;
        if(field==0)coin.height--;
        if(field==1)coin.isCoinbase=!coin.isCoinbase;
        if(field==2)coin.value=AmountUna::Una(2);
        if(field==3)coin.scriptPubKey.push_back(0x51);
        if(field==4)coin.is_confidential=true;
        if(field==5)coin.commitment={1};
        EXPECT_ANY_THROW(VerifyOrchardSnapshotCoins(f.section,coins))<<field;
    }
}
TEST(OrchardSnapshotFile, AtomicPublicationPreservesDestinationOnVerificationFailure) {
    AuthenticatedFixture f;const auto path=std::filesystem::temp_directory_path()/
        ("dinero-v6-export-"+util::hex(secure_random_bytes(16)));
    ASSERT_TRUE(std::filesystem::create_directory(path));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{path};
    const auto destination=path/"snapshot.v6";
    {std::ofstream existing(destination);existing<<"preserve previous publication";}
    SnapshotMetadata metadata;metadata.version=SNAPSHOT_VERSION_V6;metadata.block_hash=f.section.state.block_hash;
    metadata.block_height=f.section.state.height;metadata.timestamp=f.header.timestamp;metadata.utxo_count=f.coins.size();
    auto forged=f.section;forged.accumulator.push_back(0);
    EXPECT_FALSE(WriteOrchardSnapshotFileAtomically(destination,metadata,forged,f.coins).success);
    {std::ifstream existing(destination);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(existing),{}),"preserve previous publication");}
    const auto result=WriteOrchardSnapshotFileAtomically(destination,metadata,f.section,f.coins);ASSERT_TRUE(result.success)<<result.error_message;
    std::ifstream input(destination,std::ios::binary);
    const auto parsed=ReadOrchardSnapshotFile(input,std::filesystem::file_size(destination),OrchardSnapshotProfile::Full);
    EXPECT_EQ(parsed.checksum,result.checksum);EXPECT_EQ(parsed.bytes,result.bytes_written);
    size_t files=0;for(const auto& entry:std::filesystem::directory_iterator(path)){(void)entry;++files;}EXPECT_EQ(files,1u);
}
TEST(OrchardSnapshotFile, DiskMetadataWalkRequiresExactHeightCoinbaseAndCoverage) {
    AuthenticatedFixture f;
    for(unsigned fault=0;fault<5;++fault) {
        auto section=f.section;section.parent_catalog.leaf_activation=section.state.height+1;
        section.base_catalog.leaf_activation=section.parent_catalog.leaf_activation;
        section.base_catalog.previous_record=c::Hash(section.parent_catalog.Encode());
        c::Tree metadata(c::Kind::LegacyCoins,[&](const auto& id){return section.nodes.at(id);},
            [&](const auto& id,const auto& bytes){section.nodes.emplace(id,bytes);});
        for(const auto& [point,coin]:f.coins) {
            if(fault==3)continue;
            auto named=point;if(fault==4)++named.vout;
            const auto point_bytes=c::OutpointBytes(named);auto value=point_bytes;
            c::Number(value,coin.height+(fault==1),4);value.push_back(uint8_t(coin.isCoinbase)^(fault==2));
            section.base_catalog.legacy=metadata.Insert(section.base_catalog.legacy,c::LegacyKey(point_bytes),value);
            ++section.base_catalog.legacy_count;
        }
        const auto disk=DiskSection(std::move(section));
        if(!fault)EXPECT_NO_THROW(VerifyOrchardSnapshotCoins(disk,f.coins));
        else EXPECT_ANY_THROW(VerifyOrchardSnapshotCoins(disk,f.coins))<<fault;
    }
}
TEST(OrchardSnapshotForest, CanonicalDeletionAndHostileLeafCount) {
    UtreexoForest forest;const UtreexoHash first(32,1),second(32,2),third(32,3);
    forest.add(first);forest.add(second);forest.add(third);ASSERT_TRUE(forest.removeAtKnownPosition(1,second));
    const auto canonical=CanonicalOrchardSnapshotForest(forest.serialize());
    EXPECT_EQ(forest.serializeCanonicalLeaves(ORCHARD_SNAPSHOT_MAX_SECTION_BYTES),canonical);
    EXPECT_ANY_THROW(forest.serializeCanonicalLeaves(canonical.size()-1));
    const auto decoded=DecodeOrchardSnapshotForest(canonical);
    EXPECT_EQ(decoded.getCommitment(),forest.getCommitment());EXPECT_EQ(decoded.getNumLeaves(),3u);
    EXPECT_TRUE(decoded.isDeleted(1));EXPECT_EQ(decoded.serialize(),canonical);
    auto hostile=canonical;ASSERT_GE(hostile.size(),10u);
    // v3, canonical flag, little-endian numLeaves. Refuse before rebuilding
    // any recursive tree or allocating from this attacker-controlled count.
    std::fill_n(hostile.begin()+2,8,uint8_t(255));EXPECT_ANY_THROW(DecodeOrchardSnapshotForest(hostile));
    hostile=canonical;hostile.push_back(0);EXPECT_ANY_THROW(DecodeOrchardSnapshotForest(hostile));
    for(size_t length=0;length<canonical.size();++length)
        EXPECT_ANY_THROW(DecodeOrchardSnapshotForest(std::vector<uint8_t>(canonical.begin(),canonical.begin()+length)));
}
TEST(OrchardSnapshotForest, DirectLeafEncodingMatchesCanonicalEncodingForEverySmallDeletionSubset) {
    for(const bool canonical_roots:{false,true})for(unsigned leaves=0;leaves<=7;++leaves)
        for(unsigned deleted=0;deleted<(1u<<leaves);++deleted) {
            // Pre-canonical fully deleted subtrees are not reconstructible
            // from leaves (the existing strict normalizer refuses them too).
            // Orchard uses canonical roots; cover every deletion subset there
            // and the legacy flag only on its undeleted reconstructible state.
            if(!canonical_roots&&deleted)continue;
            SCOPED_TRACE(::testing::Message()<<"canonical="<<canonical_roots<<" leaves="<<leaves<<" deleted="<<deleted);
            UtreexoForest forest;forest.setCanonicalEmptyRoots(canonical_roots);
            for(unsigned i=0;i<leaves;++i)forest.add(UtreexoHash(32,uint8_t(i+1)));
            for(unsigned i=0;i<leaves;++i)if(deleted&(1u<<i))
                ASSERT_TRUE(forest.removeAtKnownPosition(i,UtreexoHash(32,uint8_t(i+1))));
            const auto before=forest.getCommitment();
            std::vector<uint8_t> expected;
            ASSERT_NO_THROW(expected=CanonicalOrchardSnapshotForest(forest.serialize()));
            EXPECT_EQ(forest.serializeCanonicalLeaves(expected.size()),expected);
            EXPECT_ANY_THROW(forest.serializeCanonicalLeaves(expected.size()-1));
            EXPECT_EQ(forest.getCommitment(),before);
            const auto restored=DecodeOrchardSnapshotForest(expected);
            EXPECT_EQ(restored.getCommitment(),before);EXPECT_EQ(restored.getActiveLeaves(),leaves-std::popcount(deleted));
        }
}
TEST(OrchardSnapshotFile, DiskCoinAuditMatchesStrictForestForEverySmallDeletionSubset) {
    SelectParams(Chain::REGTEST);
    const auto height=std::max<uint32_t>(1,GetUtreexoMaturityLeafActivationHeight());
    for(unsigned leaves=0;leaves<=6;++leaves)for(unsigned deleted=0;deleted<(1u<<leaves);++deleted) {
        SCOPED_TRACE(::testing::Message()<<"leaves="<<leaves<<" deleted="<<deleted);
        OrchardSnapshotSection section;section.state.height=height;section.base_catalog.leaf_activation=height;
        OrchardSnapshotCoins coins;UtreexoForest forest;forest.setCanonicalEmptyRoots(true);
        for(unsigned i=0;i<leaves;++i) {
            const OutPoint point(TxId(H(uint8_t(i+100))),0);UTXOEntry coin;
            coin.value=AmountUna::Una(1);coin.scriptPubKey={0x51};coin.height=height;
            const auto hash=HashUTXOForCreationHeight(point.txid.AsUint256(),0,1,coin.scriptPubKey,height,false);
            forest.add(hash);coins.emplace(point,coin);
        }
        for(unsigned i=0;i<leaves;++i)if(deleted&(1u<<i)) {
            const OutPoint point(TxId(H(uint8_t(i+100))),0);const auto& coin=coins.at(point);
            ASSERT_TRUE(forest.removeAtKnownPosition(i,HashUTXOForCreationHeight(point.txid.AsUint256(),0,1,coin.scriptPubKey,height,false)));
            coins.erase(point);
        }
        section.accumulator=forest.serializeCanonicalLeaves(ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);
        section.base_catalog.stump=UtreexoStump::fromForest(forest).serialize();
        ASSERT_NO_THROW(DecodeOrchardSnapshotForest(section.accumulator));
        ASSERT_NO_THROW(VerifyOrchardSnapshotCoins(section,coins));
        auto malformed=section;malformed.accumulator.push_back(0);
        EXPECT_ANY_THROW(VerifyOrchardSnapshotCoins(malformed,coins));
        if(leaves) {
            malformed=section;malformed.accumulator[14]^=1; // First encoded root.
            EXPECT_ANY_THROW(VerifyOrchardSnapshotCoins(malformed,coins));
        }
        if(deleted) {
            malformed=section;malformed.accumulator.back()=255; // Invalid tombstone position.
            EXPECT_ANY_THROW(VerifyOrchardSnapshotCoins(malformed,coins));
        }
        if(!coins.empty()) {auto missing=coins;missing.erase(missing.begin());EXPECT_ANY_THROW(VerifyOrchardSnapshotCoins(section,missing));}
    }
}
} // namespace
