#pragma once
#ifdef _WIN32
#include "daemon/datadir_guard.h"
#include "p2p/orchard_history_cache.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace dinero {
TEST(OrchardHistoryCache, WindowsRetainsExactPublicBytesAndRefusesReplacement){
    using Cache=p2p::OrchardHistoryCache;
    const auto root=std::filesystem::temp_directory_path()/
        ("oc-"+std::to_string(::GetCurrentProcessId())+"-"+
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    std::string error;uint256 id;id.data[0]=71;
    std::vector<uint8_t> bytes(130001,53);
    const auto read=[&](Cache& cache){std::vector<uint8_t> result;
        cache.Read('b',id,bytes.size(),[&](uint64_t size,const Cache::Source& source){
            result.resize(size);source(result);});return result;};
    {
        daemon::DatadirGuard guard;ASSERT_TRUE(guard.Acquire(root,error))<<error;Cache cache(guard);
        EXPECT_FALSE(cache.Has('b',id));
        cache.Put('b',id,bytes.size(),bytes.size(),[&](const Cache::Sink& sink){sink(bytes);});
        EXPECT_TRUE(cache.Has('b',id));EXPECT_EQ(read(cache),bytes);
        cache.Put('b',id,bytes.size(),bytes.size(),[&](const Cache::Sink& sink){sink(bytes);});
        auto changed=bytes;changed.back()^=1;
        EXPECT_ANY_THROW(cache.Put('b',id,changed.size(),changed.size(),[&](const Cache::Sink& sink){sink(changed);}));
        EXPECT_EQ(read(cache),bytes);
        auto incomplete=id;incomplete.data[0]++;
        EXPECT_ANY_THROW(cache.Put('b',incomplete,3,3,[](const Cache::Sink& sink){
            sink(std::vector<uint8_t>{1});throw std::runtime_error("interrupted producer");}));
        EXPECT_FALSE(cache.Has('b',incomplete));
    }
    {
        daemon::DatadirGuard guard;ASSERT_TRUE(guard.Acquire(root,error))<<error;Cache cache(guard);
        EXPECT_EQ(read(cache),bytes); // cold reopen uses the pinned root identity
        std::filesystem::path cell;
        for(const auto& entry:std::filesystem::directory_iterator(root/Cache::Directory))
            if(entry.path().extension()==".raw")cell=entry.path();
        ASSERT_FALSE(cell.empty());
        std::filesystem::create_hard_link(cell,root/"another-link");
        EXPECT_ANY_THROW(cache.Has('b',id));
        // The open directory handle blocks replacement on Windows. If the
        // filesystem does permit it, the pinned identity must refuse reads.
        std::error_code renamed;
        std::filesystem::rename(root/Cache::Directory,root/"old-cache",renamed);
        if(renamed){
            EXPECT_TRUE(std::filesystem::exists(root/Cache::Directory));
        }else{
            ASSERT_TRUE(std::filesystem::create_directory(root/Cache::Directory));
            EXPECT_ANY_THROW(cache.Has('b',id));
        }
    }
}
TEST(OrchardHistoryCache, WindowsLongDatadirPublishesAndColdReads){
    using Cache=p2p::OrchardHistoryCache;
    const auto parent=std::filesystem::temp_directory_path()/
        ("ol-"+std::to_string(::GetCurrentProcessId())+"-"+
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(parent));
    const auto root=parent/std::string(70,'r');
    ASSERT_TRUE(std::filesystem::create_directory(root));
    std::string error;uint256 id;id.data[0]=89;
    const std::vector<uint8_t> bytes(8193,27);
    {
        daemon::DatadirGuard guard;ASSERT_TRUE(guard.Acquire(root,error))<<error;Cache cache(guard);
        EXPECT_FALSE(cache.Has('b',id));
        cache.Put('b',id,bytes.size(),bytes.size(),[&](const Cache::Sink& sink){sink(bytes);});
        EXPECT_TRUE(cache.Has('b',id));
    }
    {
        daemon::DatadirGuard guard;ASSERT_TRUE(guard.Acquire(root,error))<<error;Cache cache(guard);
        std::vector<uint8_t> actual;
        cache.Read('b',id,bytes.size(),[&](uint64_t size,const Cache::Source& source){
            actual.resize(size);source(actual);});
        EXPECT_EQ(actual,bytes);
    }
}
} // namespace dinero
#endif
