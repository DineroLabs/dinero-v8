# ===== Compression Libraries (vendored, static) =====
# Universal compression library vendoring for RocksDB
# Supports: Snappy, LZ4, Zstd with static linking across all platforms

include(ExternalProject)

# Pin versions that are known-good with RocksDB 9.1.x
set(SNAPPY_TAG "1.2.2" CACHE STRING "Snappy git tag")
set(LZ4_TAG    "v1.9.4"  CACHE STRING "LZ4 git tag")
set(ZSTD_TAG   "v1.5.6"  CACHE STRING "Zstd git tag")

message(STATUS "Vendoring compression libraries for RocksDB:")
if(DINERO_WITH_SNAPPY)
  message(STATUS "  - Snappy: ${SNAPPY_TAG}")
endif()
if(DINERO_WITH_LZ4)
  message(STATUS "  - LZ4: ${LZ4_TAG}")
endif()
if(DINERO_WITH_ZSTD)
  message(STATUS "  - Zstd: ${ZSTD_TAG}")
endif()

# Common CMake arguments for all compression libraries
set(_COMPRESSION_COMMON_ARGS
  -DCMAKE_BUILD_TYPE=Release
  -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

# Windows-specific: Use static CRT for consistency
if(MSVC)
  list(APPEND _COMPRESSION_COMMON_ARGS
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>
  )
endif()

# Initialize prefix list for RocksDB CMAKE_PREFIX_PATH
set(DINERO_TPL_PREFIXES "" CACHE INTERNAL "Third-party install prefixes for find_package")

# ---------- Snappy ----------
if(DINERO_WITH_SNAPPY)
  set(SNAPPY_PREFIX ${CMAKE_BINARY_DIR}/_deps/snappy-install)
  
  if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64.*x86_64|x86_64.*arm64")
    # macOS Universal Binary: Build arm64 and x86_64 separately, then lipo
    set(SNAPPY_PREFIX_ARM64 "${SNAPPY_PREFIX}/arm64")
    set(SNAPPY_PREFIX_X64 "${SNAPPY_PREFIX}/x64")
    set(SNAPPY_PREFIX_UNI "${SNAPPY_PREFIX}/universal")
    
    # Build for arm64
    ExternalProject_Add(snappy_arm64
      GIT_REPOSITORY https://github.com/google/snappy.git
      GIT_TAG        ${SNAPPY_TAG}
      GIT_SHALLOW    TRUE
      SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/snappy-src-arm64
      BINARY_DIR     ${CMAKE_BINARY_DIR}/_deps/snappy-build-arm64
      UPDATE_DISCONNECTED TRUE
      CMAKE_ARGS
        ${_COMPRESSION_COMMON_ARGS}
        -DCMAKE_OSX_ARCHITECTURES=arm64
        -DCMAKE_INSTALL_PREFIX=${SNAPPY_PREFIX_ARM64}
        -DSNAPPY_BUILD_TESTS=OFF
        -DSNAPPY_BUILD_BENCHMARKS=OFF
      BUILD_COMMAND   ${CMAKE_COMMAND} --build . --config Release --parallel
      INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config Release
      LOG_CONFIGURE   TRUE
      LOG_BUILD       TRUE
      LOG_INSTALL     TRUE
    )
    
    # Build for x86_64
    ExternalProject_Add(snappy_x64
      GIT_REPOSITORY https://github.com/google/snappy.git
      GIT_TAG        ${SNAPPY_TAG}
      GIT_SHALLOW    TRUE
      SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/snappy-src-x64
      BINARY_DIR     ${CMAKE_BINARY_DIR}/_deps/snappy-build-x64
      UPDATE_DISCONNECTED TRUE
      CMAKE_ARGS
        ${_COMPRESSION_COMMON_ARGS}
        -DCMAKE_OSX_ARCHITECTURES=x86_64
        -DCMAKE_INSTALL_PREFIX=${SNAPPY_PREFIX_X64}
        -DSNAPPY_BUILD_TESTS=OFF
        -DSNAPPY_BUILD_BENCHMARKS=OFF
      BUILD_COMMAND   ${CMAKE_COMMAND} --build . --config Release --parallel
      INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config Release
      LOG_CONFIGURE   TRUE
      LOG_BUILD       TRUE
      LOG_INSTALL     TRUE
    )
    
    # Create universal binary
    add_custom_command(
      OUTPUT ${SNAPPY_PREFIX_UNI}/lib/libsnappy.a
      COMMAND ${CMAKE_COMMAND} -E make_directory ${SNAPPY_PREFIX_UNI}/lib ${SNAPPY_PREFIX_UNI}/include
      COMMAND lipo -create
              ${SNAPPY_PREFIX_ARM64}/lib/libsnappy.a
              ${SNAPPY_PREFIX_X64}/lib/libsnappy.a
              -output ${SNAPPY_PREFIX_UNI}/lib/libsnappy.a
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${SNAPPY_PREFIX_ARM64}/include ${SNAPPY_PREFIX_UNI}/include
      DEPENDS snappy_arm64 snappy_x64
      COMMENT "Creating universal Snappy binary"
    )
    
    add_custom_target(snappy_universal DEPENDS ${SNAPPY_PREFIX_UNI}/lib/libsnappy.a)
    add_custom_target(snappy_src DEPENDS snappy_universal)
    
    # Use universal prefix
    set(SNAPPY_PREFIX "${SNAPPY_PREFIX_UNI}")
  else()
    # Single architecture build
    ExternalProject_Add(snappy_src
      GIT_REPOSITORY https://github.com/google/snappy.git
      GIT_TAG        ${SNAPPY_TAG}
      GIT_SHALLOW    TRUE
      SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/snappy-src
      BINARY_DIR     ${CMAKE_BINARY_DIR}/_deps/snappy-build
      UPDATE_DISCONNECTED TRUE
      CMAKE_ARGS
        ${_COMPRESSION_COMMON_ARGS}
        -DCMAKE_INSTALL_PREFIX=${SNAPPY_PREFIX}
        -DSNAPPY_BUILD_TESTS=OFF
        -DSNAPPY_BUILD_BENCHMARKS=OFF
      BUILD_COMMAND   ${CMAKE_COMMAND} --build . --config Release --parallel
      INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config Release
      LOG_CONFIGURE   TRUE
      LOG_BUILD       TRUE
      LOG_INSTALL     TRUE
    )
  endif()
  
  # Add to prefix path
  list(APPEND DINERO_TPL_PREFIXES ${SNAPPY_PREFIX})
endif()

# ---------- LZ4 ----------
if(DINERO_WITH_LZ4)
  set(LZ4_PREFIX ${CMAKE_BINARY_DIR}/_deps/lz4-install)
  
  ExternalProject_Add(lz4_src
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        ${LZ4_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/lz4-src
    UPDATE_DISCONNECTED TRUE
    # LZ4 1.9.x: use Makefile build (CMake has version compatibility issues)
    CONFIGURE_COMMAND ""
    BUILD_COMMAND      $(MAKE) -C <SOURCE_DIR> lib
    INSTALL_COMMAND    $(MAKE) -C <SOURCE_DIR> PREFIX=${LZ4_PREFIX} install
    LOG_BUILD       TRUE
    LOG_INSTALL     TRUE
  )
  
  # Add to prefix path
  list(APPEND DINERO_TPL_PREFIXES ${LZ4_PREFIX})
endif()

# ---------- Zstd ----------
if(DINERO_WITH_ZSTD)
  set(ZSTD_PREFIX ${CMAKE_BINARY_DIR}/_deps/zstd-install)
  
  ExternalProject_Add(zstd_src
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        ${ZSTD_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/zstd-src
    BINARY_DIR     ${CMAKE_BINARY_DIR}/_deps/zstd-build
    UPDATE_DISCONNECTED TRUE
    SOURCE_SUBDIR  build/cmake
    CMAKE_ARGS
      ${_COMPRESSION_COMMON_ARGS}
      -DCMAKE_INSTALL_PREFIX=${ZSTD_PREFIX}
      -DZSTD_BUILD_PROGRAMS=OFF
      -DZSTD_BUILD_TESTS=OFF
    BUILD_COMMAND   ${CMAKE_COMMAND} --build . --config Release --parallel
    INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --config Release
    LOG_CONFIGURE   TRUE
    LOG_BUILD       TRUE
    LOG_INSTALL     TRUE
  )
  
  # Add to prefix path
  list(APPEND DINERO_TPL_PREFIXES ${ZSTD_PREFIX})
endif()

# Update the cache variable with all prefixes
set(DINERO_TPL_PREFIXES "${DINERO_TPL_PREFIXES}" CACHE INTERNAL "Third-party install prefixes for find_package")

# Helper function to choose correct library path based on platform
function(_choose_compression_lib OUT_VAR PREFIX BASENAME)
  if(WIN32)
    # Windows: Try various naming conventions
    set(_candidates
      "${PREFIX}/lib/${BASENAME}.lib"
      "${PREFIX}/lib/${BASENAME}_static.lib"
      "${PREFIX}/lib/lib${BASENAME}.lib"
    )
  else()
    # Unix: Try lib first (macOS), then lib64 (Linux)
    if(APPLE)
      set(_candidates
        "${PREFIX}/lib/lib${BASENAME}.a"
        "${PREFIX}/lib64/lib${BASENAME}.a"
      )
    else()
      set(_candidates
        "${PREFIX}/lib64/lib${BASENAME}.a"
        "${PREFIX}/lib/lib${BASENAME}.a"
      )
    endif()
  endif()
  
  foreach(candidate IN LISTS _candidates)
    if(EXISTS "${candidate}")
      set(${OUT_VAR} "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  
  # Fallback to first candidate (will be created by build)
  list(GET _candidates 0 _first)
  set(${OUT_VAR} "${_first}" PARENT_SCOPE)
endfunction()

# Create directory structures for imported targets
if(DINERO_WITH_SNAPPY)
  file(MAKE_DIRECTORY "${SNAPPY_PREFIX}/include")
  file(MAKE_DIRECTORY "${SNAPPY_PREFIX}/lib")
  file(MAKE_DIRECTORY "${SNAPPY_PREFIX}/lib64")
endif()

if(DINERO_WITH_LZ4)
  file(MAKE_DIRECTORY "${LZ4_PREFIX}/include")
  file(MAKE_DIRECTORY "${LZ4_PREFIX}/lib")
  file(MAKE_DIRECTORY "${LZ4_PREFIX}/lib64")
endif()

if(DINERO_WITH_ZSTD)
  file(MAKE_DIRECTORY "${ZSTD_PREFIX}/include")
  file(MAKE_DIRECTORY "${ZSTD_PREFIX}/lib")
  file(MAKE_DIRECTORY "${ZSTD_PREFIX}/lib64")
endif()

# Create imported targets for compression libraries
if(DINERO_WITH_SNAPPY)
  _choose_compression_lib(SNAPPY_LIB ${SNAPPY_PREFIX} snappy)
  add_library(Snappy::snappy STATIC IMPORTED GLOBAL)
  add_dependencies(Snappy::snappy snappy_src)
  set_target_properties(Snappy::snappy PROPERTIES
    IMPORTED_LOCATION "${SNAPPY_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${SNAPPY_PREFIX}/include"
  )
  message(STATUS "Snappy target configured: ${SNAPPY_LIB}")
endif()

if(DINERO_WITH_LZ4)
  _choose_compression_lib(LZ4_LIB ${LZ4_PREFIX} lz4)
  add_library(LZ4::lz4 STATIC IMPORTED GLOBAL)
  add_dependencies(LZ4::lz4 lz4_src)
  set_target_properties(LZ4::lz4 PROPERTIES
    IMPORTED_LOCATION "${LZ4_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LZ4_PREFIX}/include"
  )
  message(STATUS "LZ4 target configured: ${LZ4_LIB}")
endif()

if(DINERO_WITH_ZSTD)
  # Zstd usually installs as libzstd(.a|.lib)
  _choose_compression_lib(ZSTD_LIB ${ZSTD_PREFIX} zstd)
  add_library(zstd::libzstd_static STATIC IMPORTED GLOBAL)
  add_dependencies(zstd::libzstd_static zstd_src)
  set_target_properties(zstd::libzstd_static PROPERTIES
    IMPORTED_LOCATION "${ZSTD_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_PREFIX}/include"
  )
  message(STATUS "Zstd target configured: ${ZSTD_LIB}")
endif()

message(STATUS "Compression library vendoring configured with ${CMAKE_LIST_LENGTH} prefixes")
if(DINERO_TPL_PREFIXES)
  message(STATUS "Third-party prefixes: ${DINERO_TPL_PREFIXES}")
endif()
