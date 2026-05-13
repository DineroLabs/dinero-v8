// MinGW Compatibility Header for RocksDB
// This header ensures all necessary C++20 threading headers are included
// before any RocksDB code is compiled

#ifndef ROCKSDB_MINGW_COMPAT_H_
#define ROCKSDB_MINGW_COMPAT_H_

// Always include these for MinGW/Windows builds - even if _WIN32 isn't defined yet
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <functional>
#include <future>

// Ensure Windows macros are defined
#ifndef _WIN32
#define _WIN32 1
#endif

#ifndef WIN32
#define WIN32 1
#endif

#endif // ROCKSDB_MINGW_COMPAT_H_
