# ===== RocksDB Vendoring (ExternalProject, isolated) =====
#
# Builds RocksDB out-of-tree via ExternalProject_Add, sourced from the
# pinned submodule at third_party/rocksdb. Provides RocksDB::rocksdb as
# a STATIC IMPORTED target.
#
# Why ExternalProject instead of add_subdirectory:
#   - Isolates CMAKE_BINARY_DIR so RocksDB's configure_file sites don't
#     pollute our build root (the previous add_subdirectory path at
#     cmake/VendorRocksDBSimple.cmake required an inline patch to
#     third_party/rocksdb/CMakeLists.txt to work around CMAKE_BINARY_DIR
#     vs CMAKE_CURRENT_BINARY_DIR assumptions — ExternalProject makes that
#     patch unnecessary because RocksDB gets its own CMAKE_BINARY_DIR).
#   - Isolates RocksDB's CACHE options from ours (no FORCE overrides
#     cascading across the project).
#   - Isolates RocksDB's compile flags / sanitizer / warning settings.
#   - No target-namespace collision — ExternalProject exposes a single
#     imported target, not a soup of internal RocksDB CMake targets.
#
# Source : third_party/rocksdb submodule (pinned, no network fetch).
# Output : ${CMAKE_BINARY_DIR}/_deps/rocksdb-{build,install}/
# Config : static only, no compression, no tests/tools/benchmarks.

include(ExternalProject)

if(DINERO_USE_VENDORED_DEPS)
  message(STATUS "Using vendored RocksDB (ExternalProject from third_party/rocksdb submodule)")

  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/rocksdb/CMakeLists.txt")
    message(FATAL_ERROR "RocksDB submodule not initialized. Run: git submodule update --init --recursive")
  endif()

  set(ROCKSDB_SOURCE_DIR  "${CMAKE_SOURCE_DIR}/third_party/rocksdb")
  set(ROCKSDB_BINARY_DIR  "${CMAKE_BINARY_DIR}/_deps/rocksdb-build")
  set(ROCKSDB_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/rocksdb-install")

  # Static-library naming: librocksdb.a on MinGW/Linux/Mac, rocksdb.lib on
  # MSVC. Multi-config generators (Visual Studio, Xcode) place the build
  # output under a per-config subdir (Release/Debug/...); single-config
  # generators (Unix Makefiles, Ninja) place it directly in the build dir.
  set(_rocksdb_lib_name "${CMAKE_STATIC_LIBRARY_PREFIX}rocksdb${CMAKE_STATIC_LIBRARY_SUFFIX}")
  if(CMAKE_CONFIGURATION_TYPES)
    set(_rocksdb_built_lib "${ROCKSDB_BINARY_DIR}/Release/${_rocksdb_lib_name}")
  else()
    set(_rocksdb_built_lib "${ROCKSDB_BINARY_DIR}/${_rocksdb_lib_name}")
  endif()
  set(_rocksdb_installed_lib "${ROCKSDB_INSTALL_DIR}/lib/${_rocksdb_lib_name}")

  set(_rocksdb_cmake_args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${ROCKSDB_INSTALL_DIR}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DROCKSDB_BUILD_SHARED=OFF
    -DROCKSDB_BUILD_STATIC=ON
    -DUSE_RTTI=ON
    -DPORTABLE=ON
    -DWITH_TESTS=OFF
    -DWITH_TOOLS=OFF
    -DWITH_BENCHMARK_TOOLS=OFF
    -DWITH_EXAMPLES=OFF
    -DWITH_GFLAGS=OFF
    -DWITH_SNAPPY=OFF
    -DWITH_LZ4=OFF
    -DWITH_ZSTD=OFF
    -DWITH_ZLIB=OFF
    -DWITH_BZ2=OFF
    -DFAIL_ON_WARNINGS=OFF
    -DWITH_SYNC_POINT=OFF
  )

  # ExternalProject has an isolated CMake directory and therefore does not
  # inherit the top-level add_compile_options() reproducibility policy. Without
  # explicit prefix maps, RocksDB embeds the checkout path in DWARF and in its
  # file-name logging strings; two otherwise identical builders then produce
  # different final dinerod bytes. Map the parent source/build roots to the same
  # canonical names used by ReproducibleBuild.cmake.
  if(DINERO_REPRODUCIBLE_BUILD AND
     CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(_rocksdb_repro_flags
      "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=. -fdebug-prefix-map=${CMAKE_SOURCE_DIR}=. -fmacro-prefix-map=${CMAKE_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_BINARY_DIR}=./build -fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build -fmacro-prefix-map=${CMAKE_BINARY_DIR}=./build -fno-record-gcc-switches -Wdate-time")
    list(APPEND _rocksdb_cmake_args
      "-DCMAKE_C_FLAGS=${_rocksdb_repro_flags}"
      "-DCMAKE_CXX_FLAGS=${_rocksdb_repro_flags}"
    )
  endif()

  if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    list(APPEND _rocksdb_cmake_args -DFORCE_NO_LIBATOMIC=ON)
    if(CMAKE_OSX_SYSROOT)
      list(APPEND _rocksdb_cmake_args -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      list(APPEND _rocksdb_cmake_args -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
      list(APPEND _rocksdb_cmake_args -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      list(APPEND _rocksdb_cmake_args
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DIOS_PLATFORM=${IOS_PLATFORM}
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
      )
    endif()
    # RocksDB's macOS ranlib archive-size workaround (carried from the
    # superseded VendorRocksDBSimple path).
    list(APPEND _rocksdb_cmake_args
      "-DCMAKE_CXX_ARCHIVE_CREATE=<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>"
      "-DCMAKE_CXX_ARCHIVE_FINISH="
    )
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    list(APPEND _rocksdb_cmake_args
      -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
      -DANDROID_ABI=${ANDROID_ABI}
      -DANDROID_PLATFORM=${ANDROID_PLATFORM}
      -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
      "-DCMAKE_CXX_FLAGS=-include ${CMAKE_SOURCE_DIR}/cmake/android_rocksdb_compat.h"
    )
  endif()

  set(_rocksdb_build_parallel 8)
  if(MSVC)
    # RocksDB compiles many translation units into one static library target.
    # Visual Studio parallel builds otherwise race on rocksdb.pdb and fail with
    # C1041. /FS asks cl.exe to serialize PDB writes while keeping parallelism.
    list(APPEND _rocksdb_cmake_args "-DCMAKE_CXX_FLAGS=/FS")
    # Some MSVC/RocksDB combinations still trip over the shared target PDB even
    # with /FS. Keep non-Windows fast, but make native Windows release builds
    # deterministic.
    set(_rocksdb_build_parallel 1)
  endif()

  ExternalProject_Add(rocksdb_external
    SOURCE_DIR        ${ROCKSDB_SOURCE_DIR}
    BINARY_DIR        ${ROCKSDB_BINARY_DIR}
    INSTALL_DIR       ${ROCKSDB_INSTALL_DIR}
    DOWNLOAD_COMMAND  ""
    UPDATE_COMMAND    ""
    CMAKE_ARGS        ${_rocksdb_cmake_args}
    BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR> --target rocksdb --config Release --parallel ${_rocksdb_build_parallel}
    INSTALL_COMMAND   ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/lib
              COMMAND ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/include
              COMMAND ${CMAKE_COMMAND} -E copy ${_rocksdb_built_lib} ${_rocksdb_installed_lib}
              COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROCKSDB_SOURCE_DIR}/include <INSTALL_DIR>/include
    BUILD_BYPRODUCTS  ${_rocksdb_installed_lib}
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
  )

  # The custom install step below controls the vendored layout, so lib/ is
  # the canonical location on every platform.
  set(ROCKSDB_LIBRARY "${_rocksdb_installed_lib}")

  # Pre-create the include dir so set_target_properties doesn't error
  # at configure time (ExternalProject populates it at build time).
  file(MAKE_DIRECTORY "${ROCKSDB_INSTALL_DIR}/include")

  add_library(RocksDB::rocksdb STATIC IMPORTED GLOBAL)
  add_dependencies(RocksDB::rocksdb rocksdb_external)
  set_target_properties(RocksDB::rocksdb PROPERTIES
    IMPORTED_LOCATION             "${ROCKSDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ROCKSDB_INSTALL_DIR}/include"
  )

  if(UNIX AND NOT APPLE)
    set_property(TARGET RocksDB::rocksdb APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES "pthread;dl"
    )
  elseif(APPLE)
    set_property(TARGET RocksDB::rocksdb APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES "pthread"
    )
  endif()

  message(STATUS "RocksDB: vendored via ExternalProject (isolated build)")
  message(STATUS "  source  : ${ROCKSDB_SOURCE_DIR}")
  message(STATUS "  build   : ${ROCKSDB_BINARY_DIR}")
  message(STATUS "  install : ${ROCKSDB_INSTALL_DIR}")

else()
  message(STATUS "Using system RocksDB via find_package")
  find_package(RocksDB REQUIRED)
  message(STATUS "RocksDB: Using system installation")
endif()
