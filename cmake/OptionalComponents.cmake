# Optional in-tree component build switches.
#
# Included from the repository root after the core daemon/CLI/test wiring.
# Keep miner before Qt: qt/CMakeLists.txt checks whether dinero-solo-miner
# already exists before deciding whether to add its fallback miner subdirectory.

# Solo miner subdirectory (Phase 2 of monorepo consolidation, 2026-05-12)
option(DINERO_BUILD_MINER "Build dinero-solo-miner library + CLI as in-tree subdirectory (Phase 2 monorepo)" OFF)
if(DINERO_BUILD_MINER)
  message(STATUS "dinero-solo-miner build: ENABLED (DINERO_BUILD_MINER=ON)")
  add_subdirectory(miner)
else()
  message(STATUS "dinero-solo-miner build: disabled (pass -DDINERO_BUILD_MINER=ON to enable)")
endif()

# Qt GUI subdirectory (Phase 2 of monorepo consolidation, 2026-05-12)
option(DINERO_BUILD_QT "Build dinero-qt GUI alongside dinerod (Phase 2 monorepo)" OFF)
if(DINERO_BUILD_QT)
  message(STATUS "dinero-qt GUI build: ENABLED (DINERO_BUILD_QT=ON)")
  add_subdirectory(qt)
else()
  message(STATUS "dinero-qt GUI build: disabled (pass -DDINERO_BUILD_QT=ON to enable)")
endif()

# Seeder subdirectory (Phase E of v8 peer-discovery, 2026-05-12)
option(DINERO_BUILD_SEEDER "Build dinero-seeder peer-discovery crawler (Phase E)" OFF)
if(DINERO_BUILD_SEEDER)
  message(STATUS "dinero-seeder build: ENABLED (DINERO_BUILD_SEEDER=ON)")
  add_subdirectory(seeder)
else()
  message(STATUS "dinero-seeder build: disabled (pass -DDINERO_BUILD_SEEDER=ON to enable)")
endif()
