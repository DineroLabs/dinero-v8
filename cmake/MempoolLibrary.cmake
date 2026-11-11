# The unconfirmed-transaction engine and its policy helpers. Node adapters
# live outside this library; all consumers share this single compilation.
add_library(dinero_mempool STATIC
  src/daemon/mempool.cpp
  src/mempool/coins_view_mempool.cpp
  src/mempool/mempool_persistence.cpp
  src/mempool/fee_estimator.cpp
  src/mempool/invalid_tx_cache.cpp
  src/mempool/tx_orphan_pool.cpp
  src/policy/mempool_policy.cpp
  src/policy/rbf_policy.cpp
  src/policy/fee_estimator.cpp
  src/daemon/relay_transaction_reader.cpp
  src/mining/ct_selection_policy.cpp
  src/mining/address_validator.cpp
)
target_compile_features(dinero_mempool PUBLIC cxx_std_20)
target_include_directories(dinero_mempool PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(dinero_mempool PUBLIC
  dinero_consensus dinero_consensus_core dinero_tx_primitives dinero_pq
  dinero_shielded dinero_crypto dinero_address_encoding dinero_logging)
# dinero_shielded still inherits legacy SQLite/note-store dependencies. Keep
# that declared debt visible; this library is not yet fully storage-free.
