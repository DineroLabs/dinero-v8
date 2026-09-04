# Dinero v7 Shielded Pool — commitment tree, nullifier set, validation.
#
# Architecturally separate from Utreexo. No code path in this library
# inserts a shielded commitment into the Utreexo accumulator.
#
# Uses tagged-SHA256 as a placeholder for Poseidon. When the Poseidon
# gadget is wired into the proof system, swap the hash function in
# commitment_tree.cpp — all interfaces remain identical.

add_library(dinero_shielded STATIC
    anchor_history.cpp
    commitment_tree.cpp
    nullifier_set.cpp
    shielded_epoch.cpp
    binding_sig.cpp
    bundle_builder.cpp
    pedersen_commit.cpp
    pedersen_generators.cpp
    range_proof.cpp
    shielded_validation.cpp
    shielded_commitment.cpp
    shielded_circuit.cpp
    shielded_serialization.cpp
    shielded_output_feed.cpp
    shielded_witness.cpp
    shielded_block_validation.cpp
    shielded_block_section.cpp
    # Wallet-layer shielded operations (note store + shield/unshield handlers).
    # Lives in dinero_shielded rather than dinero_wallet to avoid circular deps.
    ${CMAKE_SOURCE_DIR}/src/wallet/shielded_derivation.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/shielded_note_store.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/shielded_wallet_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/wallet_routing_engine.cpp
)

target_include_directories(dinero_shielded PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# poseidon_gadget.h lives in src/zk/zkvm/ (not a public include).
target_include_directories(dinero_shielded PRIVATE
    ${CMAKE_SOURCE_DIR}/src/zk/zkvm
)

target_link_libraries(dinero_shielded PRIVATE
    dinero_zk         # Poseidon-2 native evaluator (poseidon2_bytes)
    dinero_crypto     # CSHA256 sighash construction
    sqlite3           # NullifierSet persistence
)

set_target_properties(dinero_shielded PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
