#pragma once
#include <stdexcept>
#include <string>

namespace din {

/**
 * @brief Macro for marking TODO items that should throw at runtime
 * 
 * Use this for placeholders that need to be implemented but shouldn't
 * break builds. The function will throw a clear error message when called.
 * 
 * Example:
 *   auto Miner::enableTurbo() -> void { DIN_TODO("implement Miner::enableTurbo"); }
 */
#define DIN_TODO(msg) throw std::runtime_error(std::string("TODO: ") + msg)

/**
 * @brief Macro for optional features that require specific build flags
 * 
 * Use this to guard features that are only available when certain
 * build options are enabled.
 * 
 * Example:
 *   void UtxoDb::open(...) {
 *     DIN_REQUIRE_ROCKSDB();
 *     // real code when enabled
 *   }
 */

#if !defined(DIN_WITH_ROCKSDB) || !DIN_WITH_ROCKSDB
  #define DIN_REQUIRE_ROCKSDB() \
    throw std::runtime_error("RocksDB disabled in this build")
#else
  #define DIN_REQUIRE_ROCKSDB() (void)0
#endif

#if !defined(DIN_BUILD_GUI) || !DIN_BUILD_GUI
  #define DIN_REQUIRE_GUI() \
    throw std::runtime_error("GUI disabled in this build")
#else
  #define DIN_REQUIRE_GUI() (void)0
#endif

#if !defined(DIN_ENABLE_P2P) || !DIN_ENABLE_P2P
  #define DIN_REQUIRE_P2P() \
    throw std::runtime_error("P2P layer disabled in this build")
#else
  #define DIN_REQUIRE_P2P() (void)0
#endif

} // namespace din
