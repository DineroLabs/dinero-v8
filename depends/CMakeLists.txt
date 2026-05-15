cmake_minimum_required(VERSION 3.21)

project(dinero_depends LANGUAGES C)

include(ExternalProject)

set(DINERO_DEPENDS_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(DINERO_REPO_DIR "${DINERO_DEPENDS_DIR}/.." ABSOLUTE)

if(NOT DINERO_DEPENDS_HOST)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -dumpmachine
    OUTPUT_VARIABLE _compiler_host
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_compiler_host)
    set(DINERO_DEPENDS_HOST "${_compiler_host}")
  else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _system_name)
    set(DINERO_DEPENDS_HOST "${_system_name}-${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif()

set(DINERO_DEPENDS_PREFIX
    "${DINERO_DEPENDS_DIR}/${DINERO_DEPENDS_HOST}"
    CACHE PATH "Dinero dependency install prefix")
set(DINERO_DEPENDS_SOURCES_DIR
    "${DINERO_DEPENDS_DIR}/sources"
    CACHE PATH "Dinero dependency source download directory")
set(DINERO_DEPENDS_WORK_DIR
    "${DINERO_DEPENDS_DIR}/work"
    CACHE PATH "Dinero dependency build work directory")

if(CMAKE_BUILD_TYPE)
  set(DINERO_DEPENDS_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
else()
  set(DINERO_DEPENDS_BUILD_TYPE Release)
endif()

set(DINERO_DEPENDS_COMMON_CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_BUILD_TYPE=${DINERO_DEPENDS_BUILD_TYPE}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND DINERO_DEPENDS_COMMON_CMAKE_ARGS
       -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()

if(CMAKE_OSX_DEPLOYMENT_TARGET)
  list(APPEND DINERO_DEPENDS_COMMON_CMAKE_ARGS
       -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
elseif(APPLE AND EXISTS "${DINERO_REPO_DIR}/.macos-deployment-target")
  file(STRINGS "${DINERO_REPO_DIR}/.macos-deployment-target" _macos_target LIMIT_COUNT 1)
  list(APPEND DINERO_DEPENDS_COMMON_CMAKE_ARGS
       -DCMAKE_OSX_DEPLOYMENT_TARGET=${_macos_target})
endif()

if(CMAKE_OSX_ARCHITECTURES)
  list(APPEND DINERO_DEPENDS_COMMON_CMAKE_ARGS
       -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})
endif()

message(STATUS "Dinero depends host: ${DINERO_DEPENDS_HOST}")
message(STATUS "Dinero depends prefix: ${DINERO_DEPENDS_PREFIX}")
message(STATUS "Dinero depends sources: ${DINERO_DEPENDS_SOURCES_DIR}")
message(STATUS "Dinero depends work: ${DINERO_DEPENDS_WORK_DIR}")

include("${DINERO_DEPENDS_DIR}/packages/miniupnpc.cmake")
include("${DINERO_DEPENDS_DIR}/packages/libnatpmp.cmake")

add_custom_target(depends-current ALL
  COMMAND ${CMAKE_COMMAND}
          -DDINERO_DEPENDS_DIR=${DINERO_DEPENDS_DIR}
          -DDINERO_DEPENDS_PREFIX=${DINERO_DEPENDS_PREFIX}
          -P ${DINERO_DEPENDS_DIR}/cmake/WriteCurrentPrefix.cmake
  DEPENDS miniupnpc libnatpmp
  COMMENT "Writing Dinero depends/current prefix marker")

add_custom_target(print-prefix
  COMMAND ${CMAKE_COMMAND} -E echo "${DINERO_DEPENDS_PREFIX}")
