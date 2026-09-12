# Optional in-tree component build switches.
#
# Included from the repository root after the core daemon/CLI/test wiring.
# Keep miner before Qt: qt/CMakeLists.txt checks whether dinero-solo-miner
# already exists before deciding whether to add its fallback miner subdirectory.

# Select the Qt platform default BEFORE miner/ creates its cache options and
# targets. Setting it in qt/ is too late when the miner is built in-tree.
option(DINERO_BUILD_QT "Build dinero-qt GUI alongside dinerod (Phase 2 monorepo)" OFF)
if(APPLE AND DINERO_BUILD_QT AND NOT DEFINED MINER_ENABLE_METAL)
  set(MINER_ENABLE_METAL ON CACHE BOOL
      "Enable the embedded Metal solo-mining backend for Dinero Qt")
endif()

# Solo miner subdirectory (Phase 2 of monorepo consolidation, 2026-05-12)
option(DINERO_BUILD_MINER "Build dinero-solo-miner library + CLI as in-tree subdirectory (Phase 2 monorepo)" OFF)
if(DINERO_BUILD_MINER)
  message(STATUS "dinero-solo-miner build: ENABLED (DINERO_BUILD_MINER=ON)")
  add_subdirectory(miner)
else()
  message(STATUS "dinero-solo-miner build: disabled (pass -DDINERO_BUILD_MINER=ON to enable)")
endif()

# Seeder subdirectory (Phase E of v8 peer-discovery, 2026-05-12)
option(DINERO_BUILD_SEEDER "Build dinero-seeder peer-discovery crawler (Phase E)" OFF)
if(DINERO_BUILD_SEEDER)
  message(STATUS "dinero-seeder build: ENABLED (DINERO_BUILD_SEEDER=ON)")
  add_subdirectory(seeder)
else()
  message(STATUS "dinero-seeder build: disabled (pass -DDINERO_BUILD_SEEDER=ON to enable)")
endif()

# Qt GUI subdirectory (Phase 2 of monorepo consolidation, 2026-05-12)
# Keep components Qt embeds before Qt so qt/CMakeLists.txt can detect their
# targets when installing self-contained app bundles.
if(DINERO_BUILD_QT)
  message(STATUS "dinero-qt GUI build: ENABLED (DINERO_BUILD_QT=ON)")
  add_subdirectory(qt)
else()
  message(STATUS "dinero-qt GUI build: disabled (pass -DDINERO_BUILD_QT=ON to enable)")
endif()
