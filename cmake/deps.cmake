# ===== DineroCoin Dependencies Central Switchboard =====
# This file manages all third-party dependencies for DineroCoin
# Controlled by DINERO_USE_VENDORED_DEPS option:
#   ON  (default) → Use vendored submodules (CI, releases, reproducible builds)
#   OFF           → Use system packages (local development)

message(STATUS "")
message(STATUS "===== Dependency Configuration =====")
if(DINERO_USE_VENDORED_DEPS)
  message(STATUS "Mode: VENDORED (reproducible, hermetic builds)")
  message(STATUS "Source: git submodules in third_party/")
else()
  message(STATUS "Mode: SYSTEM PACKAGES (local development)")
  message(STATUS "Source: apt/brew/choco package managers")
endif()
message(STATUS "")

# RocksDB (key-value database)
include(cmake/VendorRocksDB.cmake)

# GoogleTest (unit testing framework)
include(cmake/VendorGTest.cmake)

# Boost (header-only: Asio, Beast, Endian)
include(cmake/VendorBoost.cmake)

# Note: Other dependencies (jsoncpp, blake3, libwally, etc.) are already
# vendored via third_party/ subdirectories and don't need conditional handling

message(STATUS "===== Dependency Configuration Complete =====")
message(STATUS "")
