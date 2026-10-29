# Wallet/consensus separation gate probes (K1/W1; K3/W3 via check_symbols.py).
# Enabled only with -DDINERO_SEPARATION_GATES=ON. See
# docs/superpowers/specs/2026-10-29-wallet-node-separation-design.md section 8.
#
# Each probe links ONE root library whole-archive, plus exactly the link
# dependencies CMake propagates from that library (its INTERFACE_LINK_LIBRARIES)
# and nothing else. Whole-archive pulls in every object, so an undeclared
# dependency fails the link every time instead of depending on link order.
#
# Both targets are EXCLUDE_FROM_ALL so full builds stay green while the probes
# are RED; their ctests build the target first, then run it.

function(dinero_sep_forced_link target lib source)
  add_executable(${target} EXCLUDE_FROM_ALL ${source})
  target_compile_features(${target} PRIVATE cxx_std_20)
  # Plain link: brings ${lib} and its declared (propagated) deps, nothing more.
  target_link_libraries(${target} PRIVATE ${lib})
  if(MSVC)
    target_link_options(${target} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE_NAME:${lib}>")
  elseif(APPLE)
    # ld64: undefined symbols are already errors for executables.
    target_link_options(${target} PRIVATE "-Wl,-force_load,$<TARGET_FILE:${lib}>")
  else()
    target_link_options(${target} PRIVATE
      "-Wl,--whole-archive" "$<TARGET_FILE:${lib}>" "-Wl,--no-whole-archive"
      "-Wl,--no-undefined")
  endif()
endfunction()

dinero_sep_forced_link(sep_consensus_forced_link dinero_consensus consensus_forced_link.cpp)
dinero_sep_forced_link(sep_wallet_forced_link dinero_wallet wallet_forced_link.cpp)

set(_sep_driver ${CMAKE_CURRENT_SOURCE_DIR}/run_forced_link.cmake)
foreach(_pair "SepConsensusForcedLink;sep_consensus_forced_link"
              "SepWalletForcedLink;sep_wallet_forced_link")
  list(GET _pair 0 _test)
  list(GET _pair 1 _tgt)
  add_test(NAME ${_test} COMMAND ${CMAKE_COMMAND}
    -DSEP_BUILD_DIR=${CMAKE_BINARY_DIR}
    -DSEP_TARGET=${_tgt}
    -DSEP_CONFIG=$<CONFIG>
    -DSEP_EXE=$<TARGET_FILE:${_tgt}>
    -P ${_sep_driver})
  # One cmake --build per build dir at a time (ninja is not re-entrant);
  # later build-driving separation tests join the same lock.
  set_tests_properties(${_test} PROPERTIES LABELS "separation" TIMEOUT 1800
    RESOURCE_LOCK sep_build_dir)
endforeach()
