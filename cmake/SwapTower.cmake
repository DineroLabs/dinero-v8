# dinero-swap-tower: the keyless watchtower for DIN <-> BTC swaps (holds only
# pre-signed transactions). A shipped program on macOS and Linux; not built on
# Windows (its inbox loop uses POSIX directory APIs).
#
# Installed as its own component so packagers choose whether to ship it:
#   cmake --install <build> --component swap-tower

if(NOT WIN32)
  add_executable(dinero-swap-tower
    tools/dinero_swap_tower.cpp
    src/wallet/swap/tower.cpp
    src/wallet/swap/runner.cpp
    src/wallet/swap/fee_ladder.cpp
    src/wallet/swap/din_watcher.cpp
    src/wallet/swap/btc_watcher.cpp
    src/wallet/swap/btc_tx.cpp
    src/wallet/swap/engine.cpp
    src/wallet/swap/offer.cpp
    src/wallet/swap/htlc.cpp
  )
  target_link_libraries(dinero-swap-tower PRIVATE dinero_rpc_client dinero_consensus)
  link_zstd_if_needed(dinero-swap-tower)
  target_include_directories(dinero-swap-tower PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/external)
  # With DINERO_SWAP_LIGHT_BTC the tower can also run without Bitcoin Core
  # (--btc-backend light); it then links libcurl, like that dinerod.
  if(DINERO_SWAP_LIGHT_BTC)
    target_sources(dinero-swap-tower PRIVATE
      src/wallet/swap/btc_light_chain_io.cpp
      src/wallet/swap/btc_light_backend.cpp
      src/wallet/swap/btc_light_transport.cpp
      src/wallet/swap/btc_light_watcher.cpp
      src/wallet/swap/btc_light_deposit.cpp
      src/wallet/swap/btc_deposit.cpp
      src/wallet/swap/btc_esplora.cpp
      src/wallet/swap/btc_http_curl.cpp
      src/wallet/swap/btc_merkle_proof.cpp
      src/wallet/swap/btc_raw_tx.cpp
      src/wallet/swap/btc_header_chain.cpp
      src/wallet/swap/btc_header_store.cpp
      src/wallet/swap/btc_p2p.cpp
      src/wallet/swap/btc_p2p_client.cpp)
    target_compile_definitions(dinero-swap-tower PRIVATE DINERO_SWAP_LIGHT_BTC=1)
    target_link_libraries(dinero-swap-tower PRIVATE dinero_crypto ${DINERO_CURL_TARGET})
  endif()
  install(TARGETS dinero-swap-tower DESTINATION bin COMPONENT swap-tower)

  # The door of a public tower (tools/dinero_swap_tower_intake.cpp): takes
  # packages over HTTP into the inbox a dinero-swap-tower guards. No key, no
  # chain access, no libcurl.
  add_executable(dinero-swap-tower-intake
    tools/dinero_swap_tower_intake.cpp
    src/wallet/swap/tower_http.cpp
    src/wallet/swap/tower.cpp
    src/wallet/swap/runner.cpp
    src/wallet/swap/fee_ladder.cpp
    src/wallet/swap/din_watcher.cpp
    src/wallet/swap/btc_watcher.cpp
    src/wallet/swap/btc_tx.cpp
    src/wallet/swap/engine.cpp
    src/wallet/swap/offer.cpp
    src/wallet/swap/htlc.cpp
  )
  target_link_libraries(dinero-swap-tower-intake PRIVATE dinero_rpc_client dinero_consensus)
  link_zstd_if_needed(dinero-swap-tower-intake)
  target_include_directories(dinero-swap-tower-intake PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/external)
  target_link_libraries(dinero-swap-tower-intake PRIVATE Boost::beast)
  install(TARGETS dinero-swap-tower-intake DESTINATION bin COMPONENT swap-tower)
endif()
