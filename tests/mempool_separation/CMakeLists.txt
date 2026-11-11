# Probe the actual library; no duplicate sources, core or stub providers.
add_executable(sep_mempool_forced_link EXCLUDE_FROM_ALL forced_link.cpp)
target_link_libraries(sep_mempool_forced_link PRIVATE dinero_mempool)
if(MSVC)
  target_link_options(sep_mempool_forced_link PRIVATE
    "/WHOLEARCHIVE:$<TARGET_FILE_NAME:dinero_mempool>")
elseif(APPLE)
  target_link_options(sep_mempool_forced_link PRIVATE
    "-Wl,-force_load,$<TARGET_FILE:dinero_mempool>")
else()
  target_link_options(sep_mempool_forced_link PRIVATE
    "-Wl,--whole-archive" "$<TARGET_FILE:dinero_mempool>"
    "-Wl,--no-whole-archive" "-Wl,--no-undefined")
endif()
add_test(NAME SepMempoolForcedLink COMMAND ${CMAKE_COMMAND}
  -DSEP_BUILD_DIR=${CMAKE_BINARY_DIR}
  -DSEP_TARGET=sep_mempool_forced_link
  -DSEP_CONFIG=$<CONFIG>
  -DSEP_EXE=$<TARGET_FILE:sep_mempool_forced_link>
  -P ${CMAKE_SOURCE_DIR}/tests/separation/run_forced_link.cmake)
set_tests_properties(SepMempoolForcedLink PROPERTIES
  LABELS "mempool-separation" TIMEOUT 1800 RESOURCE_LOCK sep_build_dir)

add_executable(test_chain_db_mempool_view EXCLUDE_FROM_ALL test_chain_db_mempool_view.cpp)
target_link_libraries(test_chain_db_mempool_view PRIVATE dinero_chainstate)
target_compile_features(test_chain_db_mempool_view PRIVATE cxx_std_20)
add_test(NAME ChainDbMempoolView COMMAND test_chain_db_mempool_view
  "${CMAKE_BINARY_DIR}/mempool-gate-scratch")
set_tests_properties(ChainDbMempoolView PROPERTIES LABELS "mempool-separation" TIMEOUT 60)

add_executable(sep_mempool_standalone EXCLUDE_FROM_ALL standalone.cpp)
target_link_libraries(sep_mempool_standalone PRIVATE dinero_mempool)
add_test(NAME SepMempoolStandalone COMMAND sep_mempool_standalone
  "${CMAKE_BINARY_DIR}/mempool-gate-scratch")
set_tests_properties(SepMempoolStandalone PROPERTIES LABELS "mempool-separation" TIMEOUT 60)

if(DINERO_BUILD_ORCHARD_BACKEND)
  add_executable(test_mempool_orchard_scan_sink EXCLUDE_FROM_ALL orchard_scan_sink.cpp)
  target_link_libraries(test_mempool_orchard_scan_sink PRIVATE dinero_mempool)
  add_test(NAME MempoolScanSinkOrchardAdmission COMMAND test_mempool_orchard_scan_sink
    "${CMAKE_SOURCE_DIR}/rust/orchard_backend/tests/fixtures/candidate-envelope.bin")
  set_tests_properties(MempoolScanSinkOrchardAdmission PROPERTIES
    LABELS "mempool-separation" TIMEOUT 60)
endif()
