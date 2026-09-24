cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dinero_orchard_backend LANGUAGES CXX)
  include(CTest)
endif()
# This first build integration supports native Linux/macOS. Refuse accidental
# cross compilation until the Rust target and native libraries are qualified.
if(CMAKE_CROSSCOMPILING OR WIN32 OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "Orchard backend cross/platform toolchain is not qualified yet")
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
    message(FATAL_ERROR "Orchard backend currently requires the native macOS architecture")
  endif()
endif()
find_program(ORCHARD_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
set(ORCHARD_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(ORCHARD_ARCHIVE "${ORCHARD_TARGET_DIR}/release/libdinero_orchard_backend.a")
add_custom_command(OUTPUT "${ORCHARD_ARCHIVE}"
  COMMAND ${CMAKE_COMMAND} -E env RAYON_NUM_THREADS=2
    ${ORCHARD_CARGO} build --locked --release --jobs 2
    --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    --target-dir "${ORCHARD_TARGET_DIR}"
  DEPENDS Cargo.toml Cargo.lock src/lib.rs
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
add_custom_target(dinero_orchard_rust_build DEPENDS "${ORCHARD_ARCHIVE}")
add_library(dinero_orchard_rust STATIC IMPORTED)
set_target_properties(dinero_orchard_rust PROPERTIES IMPORTED_LOCATION "${ORCHARD_ARCHIVE}")
add_dependencies(dinero_orchard_rust dinero_orchard_rust_build)
add_library(dinero_orchard_backend STATIC orchard_backend.cpp orchard_signing.cpp)
target_compile_features(dinero_orchard_backend PUBLIC cxx_std_20)
target_include_directories(dinero_orchard_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
find_package(Threads REQUIRED)
if(NOT TARGET OpenSSL::Crypto)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()
target_link_libraries(dinero_orchard_backend PUBLIC dinero_orchard_rust OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(dinero_orchard_backend PUBLIC "-framework Security" "-framework CoreFoundation")
elseif(UNIX)
  target_link_libraries(dinero_orchard_backend PUBLIC m)
endif()
option(DINERO_ORCHARD_BUILD_TESTS "Build Orchard component qualification tests" ON)
if(DINERO_ORCHARD_BUILD_TESTS)
  add_executable(test_orchard_backend tests/backend.cpp)
  target_link_libraries(test_orchard_backend PRIVATE dinero_orchard_backend)
  add_test(NAME OrchardBackendCpp COMMAND test_orchard_backend "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME OrchardBackendRust COMMAND ${ORCHARD_CARGO} test --locked --release --jobs 2
    --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml" --target-dir "${ORCHARD_TARGET_DIR}"
    -- --test-threads=1)
  set_tests_properties(OrchardBackendCpp OrchardBackendRust PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE ENVIRONMENT "RAYON_NUM_THREADS=2" LABELS "orchard;integration-staging")
endif()
