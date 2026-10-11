#pragma once
#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include "consensus/chainparams.h"
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/utilities/stackable_db.h>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <map>
#include <stdexcept>
using namespace dinero;
using namespace dinero::consensus;
namespace {
struct HeaderStartupFixture {
    std::filesystem::path path;
    HeaderStore store;
    std::vector<HeaderIndexEntry> selected;
    HeaderIndexEntry fork;
    HeaderStartupFixture():path(std::filesystem::temp_directory_path()/
        ("dinero-header-startup-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
        store(path.string()) {
        SelectParams(Chain::REGTEST);
        if (std::filesystem::exists(path) || !store.Open()) throw std::runtime_error("fixture open");
        HeaderChainSelector source;
        BlockHeader header;header.version=1;header.timestamp=1000000;header.difficulty=0x1d00ffff;
        header.ZeroReserved();
        for (int i=0;i<3;++i) {
            if (!source.AddHeader(header)) throw std::runtime_error("fixture header");
            HeaderIndexEntry copy;
            if (!source.GetHeaderCopy(header.GetHash(),copy)) throw std::runtime_error("fixture copy");
            copy.parent=nullptr;selected.push_back(copy);
            header.prev_block_hash=header.GetHash();++header.timestamp;
        }
        header=selected[1].header;header.nonce=97;
        if (!source.AddHeader(header) || !source.GetHeaderCopy(header.GetHash(),fork))
            throw std::runtime_error("fixture fork");
        fork.parent=nullptr;
    }
    void PersistAll() {
        for (const auto& entry:selected) if(!store.StoreHeader(entry))throw std::runtime_error("fixture persist");
        if(!store.StoreHeader(fork) || !store.StoreBestHeader(selected.back().hash))throw std::runtime_error("fixture fork persist");
    }
    template<class F> void Raw(F action) {
        store.Close();rocksdb::Options options;options.create_if_missing=false;rocksdb::DB* raw=nullptr;
        if(!rocksdb::DB::Open(options,path.string(),&raw).ok())throw std::runtime_error("fixture raw open");
        {std::unique_ptr<rocksdb::DB> db(raw);action(*db);}
        if(!store.Open())throw std::runtime_error("fixture reopen");
    }
    std::map<std::string,std::string> Rows() {
        std::map<std::string,std::string> rows;
        Raw([&](rocksdb::DB& db){std::unique_ptr<rocksdb::Iterator> it(db.NewIterator({}));
            for(it->SeekToFirst();it->Valid();it->Next())rows.emplace(it->key().ToString(),it->value().ToString());
            if(!it->status().ok())throw std::runtime_error("fixture iterator");});
        return rows;
    }
    std::string Key()const{return std::string("h/")+std::string(reinterpret_cast<const char*>(selected.back().hash.data),32);}
    ~HeaderStartupFixture(){store.Close();std::error_code ec;std::filesystem::remove_all(path,ec);}
};
}
TEST(CompactHeaderStartup, PreservesForksAndSeedsSelectedInMemory) {
    HeaderStartupFixture f;
    // Only a branch row is durable. Its parent is supplied by the independently
    // owned selected chain; restoration must not write those missing rows back.
    ASSERT_TRUE(f.store.StoreHeader(f.fork));const auto before=f.Rows();
    auto restored=HeaderChainSelector::RestorePreservingBranches(f.store,f.selected);
    EXPECT_EQ(restored->GetHeaderCount(),4u);HeaderIndexEntry copy;
    ASSERT_TRUE(restored->GetHeaderCopy(f.fork.hash,copy));EXPECT_EQ(copy.chainwork,f.fork.chainwork);
    EXPECT_EQ(f.Rows(),before);
    auto reopened=HeaderChainSelector::RestorePreservingBranches(f.store,f.selected);
    EXPECT_EQ(reopened->GetHeaderCount(),4u);EXPECT_EQ(f.Rows(),before);
}
TEST(CompactHeaderStartup, WrongStoredWorkAndUnknownParentRefuseWithoutRewrite) {
    HeaderStartupFixture f;f.PersistAll();auto bad=f.fork;bad.chainwork=f.selected.back().chainwork;
    ASSERT_TRUE(f.store.StoreHeader(bad));const auto before=f.Rows();
    EXPECT_THROW(HeaderChainSelector::RestorePreservingBranches(f.store,f.selected),std::exception);
    EXPECT_EQ(f.Rows(),before);
    ASSERT_TRUE(f.store.StoreHeader(f.fork));ASSERT_TRUE(f.store.DeleteHeader(f.fork.hash));
    bad=f.fork;bad.header.prev_block_hash=f.selected.back().hash;bad.prev_hash=bad.header.prev_block_hash;
    bad.hash=bad.header.GetHash();bad.height=1;
    ASSERT_TRUE(f.store.StoreHeader(bad));const auto orphan=f.Rows();
    EXPECT_THROW(HeaderChainSelector::RestorePreservingBranches(f.store,f.selected),std::exception);
    EXPECT_EQ(f.Rows(),orphan);
}
TEST(CompactHeaderStartup, MalformedLengthAndLateRowsPreserveOutput) {
    HeaderStartupFixture f;f.PersistAll();const auto original=f.Rows();const auto key=f.Key();
    for(uint32_t length:{uint32_t(0),uint32_t(65),uint32_t(0xffffffff)}) {
        f.Raw([&](rocksdb::DB& db){auto value=original.at(key);
            for(unsigned i=0;i<4;++i)value[68+i]=static_cast<char>((length>>(8*i))&255);
            if(!db.Put({},key,value).ok())throw std::runtime_error("fixture mutate");});
        const auto before=f.Rows();HeaderStore::StartupSnapshot out;
        out.headers.push_back(f.fork);out.best=f.fork.hash;
        EXPECT_FALSE(f.store.ReadStartupSnapshot(out));ASSERT_EQ(out.headers.size(),1u);
        EXPECT_EQ(out.headers.front().hash,f.fork.hash);ASSERT_TRUE(out.best);EXPECT_EQ(*out.best,f.fork.hash);
        HeaderIndexEntry copy;EXPECT_FALSE(f.store.LoadHeader(f.selected.back().hash,copy));
        EXPECT_EQ(f.Rows(),before);
    }
    f.Raw([&](rocksdb::DB& db){if(!db.Put({},key,original.at(key)).ok() ||
        !db.Put({},std::string("h/")+std::string(32,static_cast<char>(255)),"bad").ok())
        throw std::runtime_error("fixture late row");});
    HeaderStore::StartupSnapshot out;out.headers.push_back(f.fork);
    EXPECT_FALSE(f.store.ReadStartupSnapshot(out));ASSERT_EQ(out.headers.size(),1u);
    EXPECT_EQ(out.headers.front().hash,f.fork.hash);
}
TEST(CompactHeaderStartup, MissingSchemaAndAbsentBestRefuse) {
    HeaderStartupFixture f;f.PersistAll();
    uint256 absent;absent.data[0]=123;ASSERT_TRUE(f.store.StoreBestHeader(absent));
    const auto before=f.Rows();
    EXPECT_THROW(HeaderChainSelector::RestorePreservingBranches(f.store,f.selected),std::exception);
    EXPECT_EQ(f.Rows(),before);
    f.Raw([](rocksdb::DB& db){if(!db.Delete({},"h/schema").ok())throw std::runtime_error("fixture schema delete");});
    const auto missing=f.Rows();HeaderStore::StartupSnapshot out;
    EXPECT_FALSE(f.store.ReadStartupSnapshot(out));EXPECT_EQ(f.Rows(),missing);
}

namespace dinero::consensus {
struct HeaderStoreStartupTestAccess {
    struct EndErrorIterator final:rocksdb::Iterator {
        std::unique_ptr<rocksdb::Iterator> inner;
        explicit EndErrorIterator(rocksdb::Iterator* source):inner(source) {}
        bool Valid()const override{return inner->Valid();}
        void SeekToFirst()override{inner->SeekToFirst();}
        void SeekToLast()override{inner->SeekToLast();}
        void Seek(const rocksdb::Slice& key)override{inner->Seek(key);}
        void SeekForPrev(const rocksdb::Slice& key)override{inner->SeekForPrev(key);}
        void Next()override{inner->Next();}
        void Prev()override{inner->Prev();}
        rocksdb::Slice key()const override{return inner->key();}
        rocksdb::Slice value()const override{return inner->value();}
        rocksdb::Status status()const override{
            const auto status=inner->status();
            return status.ok() && !inner->Valid()?rocksdb::Status::IOError("fixture terminal read refusal"):status;
        }
    };
    struct EndErrorDB final:rocksdb::StackableDB {
        explicit EndErrorDB(rocksdb::DB* source):StackableDB(std::shared_ptr<rocksdb::DB>(source,[](auto*){})) {}
        using rocksdb::StackableDB::NewIterator;
        rocksdb::Iterator* NewIterator(const rocksdb::ReadOptions& options,rocksdb::ColumnFamilyHandle* cf)override {
            return new EndErrorIterator(GetBaseDB()->NewIterator(options,cf));
        }
    };
    static bool ReadWithTerminalError(HeaderStore& store,HeaderStore::StartupSnapshot& out) {
        // Serialized fixture only: forward all real snapshot/row operations and
        // substitute a terminal I/O status. Restore the original owner on every
        // exit. No threads, removed locks, races or production fault switches.
        auto* original=store.db_;EndErrorDB wrapper(original);
        struct Restore {HeaderStore& store;rocksdb::DB* db;~Restore(){store.db_=db;}} restore{store,original};
        store.db_=&wrapper;
        return store.ReadStartupSnapshot(out);
    }
};
}
TEST(CompactHeaderStartup, TerminalReadErrorDoesNotPublishPrefixAndRetryWorks) {
    HeaderStartupFixture f;f.PersistAll();const auto before=f.Rows();
    HeaderStore::StartupSnapshot out;out.headers.push_back(f.fork);out.best=f.fork.hash;
    EXPECT_FALSE(HeaderStoreStartupTestAccess::ReadWithTerminalError(f.store,out));
    ASSERT_EQ(out.headers.size(),1u);EXPECT_EQ(out.headers.front().hash,f.fork.hash);
    ASSERT_TRUE(out.best);EXPECT_EQ(*out.best,f.fork.hash);
    EXPECT_EQ(f.Rows(),before);ASSERT_TRUE(f.store.ReadStartupSnapshot(out));EXPECT_EQ(out.headers.size(),4u);
}
