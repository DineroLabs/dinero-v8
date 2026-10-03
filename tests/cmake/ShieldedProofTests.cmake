# Keep proof and codec coverage visible as two mandatory CTest entries.
# source_root is explicit so the registration contract can be checked using a
# disposable fixture, without modifying any source or cached build tree.
function(dinero_register_shielded_proof_tests source_root)
  foreach(required IN ITEMS
      tests/zk/test_spartan_soundness.cpp
      tests/zk/test_compact_spartan.cpp
      src/consensus/shielded/compact_spartan_codec.cpp)
    if(NOT EXISTS "${source_root}/${required}")
      message(FATAL_ERROR "Required shielded proof test source missing: ${required}")
    endif()
  endforeach()

  add_executable(test_spartan_soundness "${source_root}/tests/zk/test_spartan_soundness.cpp")
  add_executable(test_compact_spartan
    "${source_root}/tests/zk/test_compact_spartan.cpp"
    "${source_root}/src/consensus/shielded/compact_spartan_codec.cpp")
  foreach(target IN ITEMS test_spartan_soundness test_compact_spartan)
    add_dependencies(${target} gtest dinero_zk)
    target_link_libraries(${target} PRIVATE GTest::gtest GTest::gtest_main dinero_zk dinero_crypto)
    target_include_directories(${target} BEFORE PRIVATE
      "${source_root}/third_party/googletest/googletest/include"
      "${source_root}/include" "${source_root}/src")
    if(APPLE)
      target_link_libraries(${target} PRIVATE "-framework Security")
    endif()
  endforeach()
  add_test(NAME SpartanSoundness COMMAND test_spartan_soundness)
  add_test(NAME CompactSpartanCodec COMMAND test_compact_spartan)
  set_tests_properties(SpartanSoundness PROPERTIES
    LABELS "zk;shielded;spartan;soundness;mandatory" TIMEOUT 300)
  set_tests_properties(CompactSpartanCodec PROPERTIES
    LABELS "zk;shielded;spartan;codec;mandatory" TIMEOUT 300)
endfunction()
