# Network support target wiring.
#
# This file is included from the repository root CMakeLists.txt rather than
# added as a subdirectory because the legacy source list below intentionally
# uses root-relative paths.

# ─────────────────────────────────────────────────────────────────────────
# Layer 2c: Utreexo Bridge Library (extracted from daemon-only TUs)
# ─────────────────────────────────────────────────────────────────────────
# bridge_node.cpp implements Utreexo proof generation for stateless/mobile
# clients. It was previously compiled directly into ~17 test targets and
# the nodecore_ffi exe because no linkable library exposed it; consumers
# either duplicated the source or stubbed the symbols (see
# tests/stubs/daemon_test_stubs.cpp BridgeNode section). Extracting it
# here lets dinero_core declare it as a transitive dep and removes both
# the duplication and the stub seam.
add_library(dinero_bridge STATIC
  src/network/bridge_node.cpp
)
target_include_directories(dinero_bridge PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(dinero_bridge PUBLIC
  dinero_chainstate  # ChainDB + ArchivalBlockReader headers
  dinero_consensus   # consensus/outpoint.h, consensus/undo.h
  dinero_crypto      # crypto/sha256.h
)
