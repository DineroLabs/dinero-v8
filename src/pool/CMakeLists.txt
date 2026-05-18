# Mining-pool support target wiring.
#
# This file is included from the repository root CMakeLists.txt rather than
# added as a subdirectory because the legacy source list below intentionally
# uses root-relative paths.

# ─────────────────────────────────────────────────────────────────────────
# Layer 2d: Mining Pool Library (extracted from daemon-only TUs)
# ─────────────────────────────────────────────────────────────────────────
# pool_db.cpp / pool_manager.cpp / payout_calculator.cpp implement the
# pool-mining accounting subsystem. Previously duplicated across multiple
# test targets and nodecore_ffi (each compiled the cpps directly), and
# also referenced from chainstate_service.cpp inside dinero_core which
# had nothing to link against (hence the pool stubs in daemon_test_stubs.cpp).
# Deps are small and contained: consensus/subsidy.h (pure math),
# storage/chain_db.h, common/logger.h. No daemon glue, no RPC.
add_library(dinero_pool STATIC
  src/pool/pool_db.cpp
  src/pool/pool_manager.cpp
  src/pool/payout_calculator.cpp
)
target_include_directories(dinero_pool PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(dinero_pool PUBLIC
  dinero_consensus   # consensus/subsidy.h
  dinero_chainstate  # storage/chain_db.h
)
