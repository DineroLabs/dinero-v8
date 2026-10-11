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
  install(TARGETS dinero-swap-tower DESTINATION bin COMPONENT swap-tower)
endif()
