#pragma once
#include "mempool/mempool_persistence.h"
#include <fstream>
#include <iterator>
namespace dinero {
namespace {
std::vector<uint8_t> ReadRetainedPoolFile(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in.good())throw std::runtime_error("retained pool fixture read");
    return {std::istreambuf_iterator<char>(in),{}};
}
void WriteRetainedPoolFile(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.close();
    if(!out.good())throw std::runtime_error("retained pool fixture write");
}
}
TEST(OrchardDiskReadmission, UnavailableFamilyOrOwnerPreservesOriginal) {
    auto name=(std::filesystem::temp_directory_path()/"orchard_disk_refusal_XXXXXX").string();ASSERT_NE(mkdtemp(name.data()),nullptr);
    struct Cleanup {std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);}} cleanup{name};
    const auto wire=ReadRetainedPoolFile(std::filesystem::path(DINERO_WALLET_RAW_FIXTURES)/"candidate-envelope.bin");
    ASSERT_GT(wire.size(),253u);ASSERT_LT(wire.size(),65536u);
    // Actual v1 file framing with one canonical fixture body and twenty opaque
    // metadata bytes. The persistence reader checks framing before admission.
    std::vector<uint8_t> bytes{'M','E','M','P','O','O','L','V',1,0,0,0,1,0xfd};
    bytes.push_back(uint8_t(wire.size()));bytes.push_back(uint8_t(wire.size()>>8));
    bytes.insert(bytes.end(),wire.begin(),wire.end());bytes.insert(bytes.end(),20,0);
    const auto path=std::filesystem::path(name)/"mempool.dat";WriteRetainedPoolFile(path,bytes);
    const auto parsed=MempoolPersistence::load(path.string());ASSERT_EQ(parsed.size(),1u);EXPECT_EQ(parsed[0].tx_bytes,wire);
    Mempool pool(nullptr);EXPECT_FALSE(pool.loadFromDisk(path.string()));EXPECT_EQ(pool.size(),0u);
    EXPECT_FALSE(pool.saveToDisk(path.string()));EXPECT_EQ(ReadRetainedPoolFile(path),bytes);
    EXPECT_FALSE(pool.loadFromDisk(path.string()));EXPECT_EQ(ReadRetainedPoolFile(path),bytes);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardDiskReadmission, ValidMixedFileReopenAndIdempotentRetry) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);auto& pool=f.f.ingress->mempool();
    const auto historical=CanonicalPoolSurvivor(f);ASSERT_TRUE(f.f.ingress->SubmitBody(historical,TxOrigin::INTERNAL).accepted());
    const auto shield=block->Transactions()[1];const auto shield_id=shield.GetTxid().AsUint256();
    auto typed=pool.getMempoolEntry(shield_id);auto legacy=pool.getMempoolEntry(historical.GetTxid().AsUint256());ASSERT_TRUE(typed);ASSERT_TRUE(legacy);
    // Deliberately unrelated persisted metadata cannot override actual fees or height.
    typed->fee=1;typed->height=0;legacy->fee=1;legacy->height=0;
    const auto path=f.f.path/"mixed-recovery.dat";
    ASSERT_TRUE(MempoolPersistence::save({*legacy,*typed},path.string()));const auto original=ReadRetainedPoolFile(path);
    pool.clear();unsigned relayed=0;pool.setTxBroadcastCallback([&](const uint256&){++relayed;});
    ASSERT_TRUE(pool.loadFromDisk(path.string()));EXPECT_EQ(pool.size(),2u);EXPECT_EQ(relayed,0u);
    ASSERT_TRUE(pool.getMempoolEntry(shield_id));EXPECT_EQ(pool.getMempoolEntry(shield_id)->tx.Serialize(),shield.Serialize());
    EXPECT_EQ(pool.getMempoolEntry(shield_id)->fee,*shield.ExplicitFee());EXPECT_EQ(pool.getMempoolEntry(shield_id)->height,101u);
    EXPECT_EQ(ReadRetainedPoolFile(path),original);ASSERT_TRUE(pool.loadFromDisk(path.string()));EXPECT_EQ(pool.size(),2u);
    pool.clear();f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    ASSERT_TRUE(pool.loadFromDisk(path.string()));EXPECT_EQ(pool.size(),2u);EXPECT_EQ(relayed,0u);
    EXPECT_TRUE(pool.saveToDisk(path.string()));std::filesystem::remove(path);
}
TEST(OrchardDiskReadmission, MissingSelectedOwnerAndChangedRetryRemainRetained) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);auto& pool=f.f.ingress->mempool();
    const auto shield=block->Transactions()[1];const auto id=shield.GetTxid().AsUint256();const auto entry=pool.getMempoolEntry(id);ASSERT_TRUE(entry);
    const auto path=f.f.path/"owned-recovery.dat";ASSERT_TRUE(MempoolPersistence::save({*entry},path.string()));const auto original=ReadRetainedPoolFile(path);
    pool.clear();f.f.service->setChainDB(nullptr);
    EXPECT_FALSE(pool.loadFromDisk(path.string()));EXPECT_FALSE(pool.saveToDisk(path.string()));EXPECT_EQ(pool.size(),0u);
    EXPECT_EQ(ReadRetainedPoolFile(path),original);
    ASSERT_TRUE(MempoolPersistence::save({},path.string()));const auto empty=ReadRetainedPoolFile(path);
    EXPECT_FALSE(pool.loadFromDisk(path.string()));EXPECT_FALSE(pool.saveToDisk(path.string()));EXPECT_EQ(ReadRetainedPoolFile(path),empty);
    WriteRetainedPoolFile(path,original);f.f.service->setChainDB(&f.f.db);
    ASSERT_TRUE(pool.loadFromDisk(path.string()));EXPECT_EQ(pool.size(),1u);EXPECT_EQ(pool.getMempoolEntry(id)->tx.Serialize(),shield.Serialize());
    EXPECT_TRUE(pool.saveToDisk(path.string()));std::filesystem::remove(path);
}
TEST(OrchardDiskReadmission, ConfirmedRefusalSurvivesUntilRealDisconnect) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);auto& pool=f.f.ingress->mempool();
    const auto shield=block->Transactions()[1];const auto id=shield.GetTxid().AsUint256();const auto entry=pool.getMempoolEntry(id);ASSERT_TRUE(entry);
    const auto path=f.f.path/"reorg-recovery.dat";ASSERT_TRUE(MempoolPersistence::save({*entry},path.string()));const auto original=ReadRetainedPoolFile(path);
    ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());EXPECT_EQ(pool.size(),0u);
    EXPECT_FALSE(pool.loadFromDisk(path.string()));EXPECT_FALSE(pool.saveToDisk(path.string()));EXPECT_EQ(pool.size(),0u);EXPECT_EQ(ReadRetainedPoolFile(path),original);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    EXPECT_FALSE(pool.loadFromDisk(path.string()));EXPECT_EQ(ReadRetainedPoolFile(path),original);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_TRUE(pool.loadFromDisk(path.string()));EXPECT_EQ(pool.size(),1u);EXPECT_EQ(pool.getMempoolEntry(id)->tx.Serialize(),shield.Serialize());
    EXPECT_TRUE(pool.saveToDisk(path.string()));std::filesystem::remove(path);
}
#endif
} // namespace dinero
