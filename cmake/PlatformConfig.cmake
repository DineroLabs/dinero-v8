# Platform defaults, global include policy, and build timestamp setup.

if(WIN32)
  add_definitions(-D_WIN32_WINNT=0x0A00)
  add_definitions(-DWIN32_LEAN_AND_MEAN)
elseif(APPLE)
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(STATUS "iOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
  else()
    set(DINERO_MACOS_TARGET_FILE "${CMAKE_SOURCE_DIR}/.macos-deployment-target")
    if(NOT EXISTS "${DINERO_MACOS_TARGET_FILE}")
      message(FATAL_ERROR
        "Missing ${DINERO_MACOS_TARGET_FILE}. "
        "Dinero requires a checked-in canonical macOS deployment target.")
    endif()

    file(READ "${DINERO_MACOS_TARGET_FILE}" DINERO_CANONICAL_MACOS_TARGET_RAW)
    string(STRIP "${DINERO_CANONICAL_MACOS_TARGET_RAW}" DINERO_CANONICAL_MACOS_TARGET)
    if(DINERO_CANONICAL_MACOS_TARGET STREQUAL "")
      message(FATAL_ERROR
        "${DINERO_MACOS_TARGET_FILE} is empty. "
        "Set it to the repo-wide minimum supported macOS version.")
    endif()

    if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
      if(NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" VERSION_EQUAL "${DINERO_CANONICAL_MACOS_TARGET}")
        message(FATAL_ERROR
          "Dinero only supports macOS ${DINERO_CANONICAL_MACOS_TARGET} builds. "
          "Requested CMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}. "
          "Update the repo policy file or rebuild with the canonical target.")
      endif()
    endif()

    set(CMAKE_OSX_DEPLOYMENT_TARGET "${DINERO_CANONICAL_MACOS_TARGET}" CACHE STRING "Minimum macOS deployment target" FORCE)
    message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET} (repo policy)")
  endif()
endif()

include_directories(${CMAKE_SOURCE_DIR}/vendor/include)
include_directories(${CMAKE_SOURCE_DIR}/include)

# Vendored JsonCpp must take precedence over Homebrew for header ABI compatibility.
include_directories(BEFORE SYSTEM ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include)

# Keep Homebrew out of global discovery; add it per-target where explicitly needed.
if(APPLE)
  list(REMOVE_ITEM CMAKE_SYSTEM_PREFIX_PATH "/opt/homebrew")
  list(REMOVE_ITEM CMAKE_SYSTEM_INCLUDE_PATH "/opt/homebrew/include")
  list(REMOVE_ITEM CMAKE_SYSTEM_LIBRARY_PATH "/opt/homebrew/lib")
endif()

# Use SOURCE_DATE_EPOCH for deterministic timestamps when provided.
if(DEFINED ENV{SOURCE_DATE_EPOCH} AND NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
  execute_process(
    COMMAND python3 -c "import datetime; print(datetime.datetime.fromtimestamp(int($ENV{SOURCE_DATE_EPOCH}), tz=datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S+0000'))"
    OUTPUT_VARIABLE BUILD_TIME
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  message(STATUS "Build timestamp: ${BUILD_TIME} (from SOURCE_DATE_EPOCH)")
else()
  string(TIMESTAMP BUILD_TIME "%Y-%m-%dT%H:%M:%S+0000" UTC)
  message(STATUS "Build timestamp: ${BUILD_TIME} (live UTC fallback)")
endif()
