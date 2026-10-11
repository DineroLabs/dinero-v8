# Build only the real crate with the pinned toolchain. The response file is
# published only after Cargo succeeds and rustc reports its native libraries.
cmake_minimum_required(VERSION 3.20)
foreach(required CARGO RUST_TOOLCHAIN MANIFEST TARGET_DIR ARCHIVE LINK_RESPONSE BUILD_CONFIG)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Missing Orchard Rust build input: ${required}")
  endif()
endforeach()
if(NOT BUILD_CONFIG MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  message(FATAL_ERROR "Orchard MSVC qualification requires a release CRT configuration")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/NativeRustPlatform.cmake")
dinero_orchard_msvc_environment()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env RAYON_NUM_THREADS=2
    "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=-C target-feature=-crt-static"
    "${CARGO}" "+${RUST_TOOLCHAIN}" rustc --color never --locked --release --jobs 2 --lib
    --target x86_64-pc-windows-msvc --manifest-path "${MANIFEST}"
    --target-dir "${TARGET_DIR}" -- --print native-static-libs
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics)
# Preserve captured streams before message rendering or native-list parsing.
# These contain compiler/linker diagnostics, never wallet inputs.
file(WRITE "${LINK_RESPONSE}.cargo.stdout" "${output}")
file(WRITE "${LINK_RESPONSE}.cargo.stderr" "${diagnostics}")
# Retain Cargo/rustc diagnostics, including the exact ordered native link list.
message("${output}${diagnostics}")
if(NOT result STREQUAL "0" OR NOT EXISTS "${ARCHIVE}")
  message(FATAL_ERROR "Orchard MSVC Rust static-library build failed: ${result}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/MsvcRustNativeLibraries.cmake")
dinero_orchard_msvc_native_libraries("${output}${diagnostics}" response)
file(WRITE "${LINK_RESPONSE}.tmp" "${response}")
file(RENAME "${LINK_RESPONSE}.tmp" "${LINK_RESPONSE}")
