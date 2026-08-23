# Reproducible build settings are opt-in through
# DINERO_REPRODUCIBLE_BUILD=ON. The caller must export SOURCE_DATE_EPOCH;
# silently inventing a timestamp makes independently reproduced artifacts
# depend on when or how CMake happened to run.
function(dinero_enable_reproducible_build)
  message(STATUS "Reproducible build enabled (SOURCE_DATE_EPOCH=$ENV{SOURCE_DATE_EPOCH})")

  if(MSVC)
    add_compile_options(
      /Brepro
      "/pathmap:${CMAKE_SOURCE_DIR}=."
      "/pathmap:${CMAKE_BINARY_DIR}=./build"
    )
    add_link_options(/Brepro)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(
      "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
      "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=."
      "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=."
      "-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build"
      "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build"
      "-fmacro-prefix-map=${CMAKE_BINARY_DIR}=./build"
      -fno-record-gcc-switches
      -Wdate-time
    )

    if(UNIX AND NOT APPLE)
      # GNU ld build IDs are not runtime semantics and may vary by toolchain.
      add_link_options(-Wl,--build-id=none)
    endif()
  else()
    message(FATAL_ERROR
      "DINERO_REPRODUCIBLE_BUILD has no policy for compiler "
      "${CMAKE_CXX_COMPILER_ID}")
  endif()

  # Current GNU ar and llvm-ar are deterministic by default. Supplying D
  # makes the contract explicit for toolchains that accept ARFLAGS.
  set(ENV{ARFLAGS} "crD")
endfunction()
