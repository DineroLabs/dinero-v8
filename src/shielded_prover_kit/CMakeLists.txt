add_library(dinero_shielded_prover_kit STATIC
  shielded_prover_kit.cpp
)

target_include_directories(dinero_shielded_prover_kit PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

target_include_directories(dinero_shielded_prover_kit PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(dinero_shielded_prover_kit PUBLIC
  dinero_shielded
  dinero_wallet
)

set_target_properties(dinero_shielded_prover_kit PROPERTIES
  OUTPUT_NAME ShieldedProverKit
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
