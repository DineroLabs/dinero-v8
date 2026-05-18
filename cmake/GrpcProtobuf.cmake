# gRPC and Protocol Buffers (Inter-daemon Communication)
#
# Used for dinerod <-> lightningd communication. Keep this included from the
# repository root so generated targets and compatibility variables remain
# visible to daemon target wiring.

option(ENABLE_GRPC "Enable gRPC server for inter-daemon communication" OFF)

# When ENABLE_GRPC=OFF (whether forced by DINERO_RELEASE or set directly by
# the operator on the configure line), define DISABLE_GRPC so daemon_app.cpp
# (and any other call-site) skips the gRPC server-construction blocks. The
# previous behaviour only defined DISABLE_GRPC under DINERO_RELEASE — that
# meant ENABLE_GRPC=OFF without DINERO_RELEASE produced unresolved-external
# linker errors for GrpcServer::* symbols. Make the gate symmetric.
if(NOT ENABLE_GRPC)
  add_compile_definitions(DISABLE_GRPC)
endif()

# Protocol Buffers and gRPC Setup (Conditional - Dev Mode Only)
# Proto types and gRPC services require protobuf.
# When ENABLE_GRPC=OFF (release mode), skip entirely - not needed.
# Dev mode uses system protobuf (dynamic) for faster builds.
# Phase 8.4: Also skip if BUILD_DINEROD=OFF (hermetic lightningd build)

if(ENABLE_GRPC AND BUILD_DINEROD)

if(DINERO_RELEASE)
  # Vendored Protobuf (Release Mode - Zero Homebrew Dependencies)
  set(PROTOBUF_INSTALL_DIR "${CMAKE_SOURCE_DIR}/third_party/protobuf-install")
  set(PROTOBUF_LIB "${PROTOBUF_INSTALL_DIR}/lib/libprotobuf.a")
  set(PROTOBUF_PROTOC "${PROTOBUF_INSTALL_DIR}/bin/protoc")

  # Check if vendored protobuf is built
  if(NOT EXISTS "${PROTOBUF_LIB}" OR NOT EXISTS "${PROTOBUF_PROTOC}")
    message(STATUS "🔨 Vendored protobuf not found, building from source...")
    message(STATUS "   Running: ${CMAKE_SOURCE_DIR}/scripts/build-vendored-protobuf.sh")

    execute_process(
      COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-vendored-protobuf.sh"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE PROTOBUF_BUILD_RESULT
      OUTPUT_VARIABLE PROTOBUF_BUILD_OUTPUT
      ERROR_VARIABLE PROTOBUF_BUILD_ERROR
    )

    if(NOT PROTOBUF_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to build vendored protobuf:\n${PROTOBUF_BUILD_ERROR}")
    endif()

    message(STATUS "✅ Vendored protobuf built successfully")
  else()
    message(STATUS "✅ Using vendored protobuf from: ${PROTOBUF_INSTALL_DIR}")
  endif()

  # Create imported target for vendored protobuf
  add_library(protobuf::libprotobuf STATIC IMPORTED GLOBAL)
  set_target_properties(protobuf::libprotobuf PROPERTIES
    IMPORTED_LOCATION "${PROTOBUF_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${PROTOBUF_INSTALL_DIR}/include"
  )

  # Also need libprotobuf-lite and libabsl (bundled with protobuf)
  set(PROTOBUF_LITE_LIB "${PROTOBUF_INSTALL_DIR}/lib/libprotobuf-lite.a")
  if(EXISTS "${PROTOBUF_LITE_LIB}")
    add_library(protobuf::libprotobuf-lite STATIC IMPORTED GLOBAL)
    set_target_properties(protobuf::libprotobuf-lite PROPERTIES
      IMPORTED_LOCATION "${PROTOBUF_LITE_LIB}"
    )
  endif()

  # Link vendored abseil libraries (built with protobuf)
  file(GLOB ABSEIL_LIBS "${PROTOBUF_INSTALL_DIR}/lib/libabsl_*.a")
  if(ABSEIL_LIBS)
    list(LENGTH ABSEIL_LIBS ABSEIL_COUNT)
    # Link abseil libs to protobuf target (abseil is a protobuf dependency)
    target_link_libraries(protobuf::libprotobuf INTERFACE ${ABSEIL_LIBS})
    message(STATUS "   Abseil libs: ${ABSEIL_COUNT} static libraries")
  endif()

  # Also need utf8_range and utf8_validity (protobuf dependencies)
  set(UTF8_RANGE_LIB "${PROTOBUF_INSTALL_DIR}/lib/libutf8_range.a")
  set(UTF8_VALIDITY_LIB "${PROTOBUF_INSTALL_DIR}/lib/libutf8_validity.a")
  if(EXISTS "${UTF8_RANGE_LIB}")
    target_link_libraries(protobuf::libprotobuf INTERFACE "${UTF8_RANGE_LIB}")
  endif()
  if(EXISTS "${UTF8_VALIDITY_LIB}")
    target_link_libraries(protobuf::libprotobuf INTERFACE "${UTF8_VALIDITY_LIB}")
  endif()

  # On macOS, abseil needs CoreFoundation framework for time zone operations
  if(APPLE)
    target_link_libraries(protobuf::libprotobuf INTERFACE "-framework CoreFoundation")
  endif()

  # Set protoc executable
  set(Protobuf_PROTOC_EXECUTABLE "${PROTOBUF_PROTOC}")
  set(Protobuf_INCLUDE_DIRS "${PROTOBUF_INSTALL_DIR}/include")

  message(STATUS "   Library: ${PROTOBUF_LIB}")
  message(STATUS "   Protoc: ${PROTOBUF_PROTOC}")

else()
  # System protobuf (dev mode)
  if(NOT TARGET protobuf::libprotobuf)
    # Try CONFIG mode first, fall back to MODULE mode (for Ubuntu systems)
    find_package(Protobuf CONFIG QUIET)
    if(NOT Protobuf_FOUND AND NOT protobuf_FOUND)
      find_package(Protobuf REQUIRED)
      # MODULE mode doesn't create targets - create them for compatibility
      if(NOT TARGET protobuf::libprotobuf AND Protobuf_LIBRARIES)
        add_library(protobuf::libprotobuf INTERFACE IMPORTED)
        set_target_properties(protobuf::libprotobuf PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${Protobuf_INCLUDE_DIRS}"
          INTERFACE_LINK_LIBRARIES "${Protobuf_LIBRARIES}"
        )
      endif()
    endif()
    if(Protobuf_FOUND OR protobuf_FOUND)
      message(STATUS "Found Protobuf ${Protobuf_VERSION}")
      if(TARGET protobuf::protoc)
        get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELEASE)
      endif()
      if(NOT Protobuf_PROTOC_EXECUTABLE)
        find_program(Protobuf_PROTOC_EXECUTABLE protoc)
      endif()
      message(STATUS "  Compiler: ${Protobuf_PROTOC_EXECUTABLE}")
      # Set compatibility variables for old-style usage
      if(NOT DEFINED Protobuf_LIBRARIES)
        set(Protobuf_LIBRARIES protobuf::libprotobuf)
      endif()
      if(NOT DEFINED Protobuf_INCLUDE_DIR)
        if(TARGET protobuf::libprotobuf)
          get_target_property(Protobuf_INCLUDE_DIR protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
        endif()
      endif()
    endif()
  else()
    message(STATUS "Using Protobuf from RocksDB dependencies")
    find_program(Protobuf_PROTOC_EXECUTABLE protoc)
  endif()
endif()

# Proto Code Generation (L1 Daemon Only - Phase 8.4: Build Graph Isolation)
# IMPORTANT: This entire section is guarded by BUILD_DINEROD
# If building lightningd standalone, NO proto generation happens
# This enforces hermetic L2 build: lightningd must not touch L1 artifacts

if(BUILD_DINEROD)
  # Proto files used by both dev and release builds (for type definitions)
  set(PROTO_FILES
    ${CMAKE_SOURCE_DIR}/proto/dinerod.proto
  )

# Set output directory for generated files
set(PROTO_OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated/proto)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR})

# Generate protobuf type definitions (ALWAYS - both dev and release modes need these)
set(GENERATED_SOURCES "")
foreach(PROTO_FILE ${PROTO_FILES})
  get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
  set(PROTO_SRC "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.pb.cc")
  set(PROTO_HDR "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.pb.h")

  # Generate protobuf code (types only - no gRPC services yet)
  add_custom_command(
    OUTPUT ${PROTO_SRC} ${PROTO_HDR}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --cpp_out=${PROTO_OUTPUT_DIR}
         --proto_path=${CMAKE_SOURCE_DIR}/proto
         ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating protobuf C++ code for ${PROTO_NAME}"
  )

  list(APPEND GENERATED_SOURCES ${PROTO_SRC})
endforeach()

# gRPC Setup (Conditional - Dev Mode Only)

if(ENABLE_GRPC)

  # Find gRPC using pkg-config (avoids protobuf target conflicts with RocksDB)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GRPC REQUIRED grpc++ grpc)
    if(GRPC_FOUND)
      message(STATUS "Found gRPC via pkg-config")
      message(STATUS "  Include dirs: ${GRPC_INCLUDE_DIRS}")
      message(STATUS "  Libraries: ${GRPC_LIBRARIES}")
    endif()
  else()
    message(FATAL_ERROR "pkg-config not found - required for gRPC")
  endif()

  # Find grpc_cpp_plugin
  find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
  if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
  endif()

  # Generate gRPC service code (only in dev mode)
  foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(PROTO_HDR "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_SRC "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_HDR "${PROTO_OUTPUT_DIR}/${PROTO_NAME}.grpc.pb.h")

    # Generate gRPC code (depends on protobuf code)
    add_custom_command(
      OUTPUT ${GRPC_SRC} ${GRPC_HDR}
      COMMAND ${Protobuf_PROTOC_EXECUTABLE}
      ARGS --grpc_out=${PROTO_OUTPUT_DIR}
           --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
           --proto_path=${CMAKE_SOURCE_DIR}/proto
           ${PROTO_FILE}
      DEPENDS ${PROTO_FILE} ${PROTO_HDR}
      COMMENT "Generating gRPC C++ code for ${PROTO_NAME}"
    )

    list(APPEND GENERATED_SOURCES ${GRPC_SRC})
  endforeach()

  # Create library with generated sources (both proto types + gRPC services)
  add_library(dinerod_proto STATIC ${GENERATED_SOURCES})

  target_include_directories(dinerod_proto PUBLIC
    ${PROTO_OUTPUT_DIR}
    ${Protobuf_INCLUDE_DIRS}
  )

  target_link_libraries(dinerod_proto PUBLIC
    protobuf::libprotobuf
  )

  # Link gRPC (using pkg-config variables)
  target_include_directories(dinerod_proto PUBLIC ${GRPC_INCLUDE_DIRS})
  target_link_directories(dinerod_proto PUBLIC ${GRPC_LIBRARY_DIRS})
  target_link_libraries(dinerod_proto PUBLIC ${GRPC_LIBRARIES})

  message(STATUS "✅ gRPC Server: ENABLED")
  message(STATUS "   Proto files: dinerod.proto")
  message(STATUS "   Generated code: ${PROTO_OUTPUT_DIR}")
else()
  # Release mode: Only proto types generated (no gRPC services)
  message(STATUS "gRPC Server: DISABLED (using socket transport)")

  # Create library with just proto type definitions
  add_library(dinerod_proto STATIC ${GENERATED_SOURCES})

  target_include_directories(dinerod_proto PUBLIC
    ${PROTO_OUTPUT_DIR}
    ${Protobuf_INCLUDE_DIRS}
  )

  target_link_libraries(dinerod_proto PUBLIC
    protobuf::libprotobuf
  )

  message(STATUS "   Proto types: dinerod.pb.h/cc (for type definitions)")
  message(STATUS "   Generated code: ${PROTO_OUTPUT_DIR}")
endif()  # ENABLE_GRPC

endif()  # BUILD_DINEROD (Phase 8.4: L1-only proto generation)

endif() # ENABLE_GRPC
