# Git and generated version metadata.

find_package(Git QUIET)

if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --git-dir
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_DIR_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(GIT_DIR_RAW)
    if(IS_ABSOLUTE "${GIT_DIR_RAW}")
      set(GIT_DIR_PATH "${GIT_DIR_RAW}")
    else()
      get_filename_component(GIT_DIR_PATH "${CMAKE_SOURCE_DIR}/${GIT_DIR_RAW}" ABSOLUTE)
    endif()
  endif()

  execute_process(COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_HEAD_REF OUTPUT_STRIP_TRAILING_WHITESPACE)

  set(GIT_METADATA_DEPENDS)
  if(GIT_DIR_PATH)
    foreach(_git_meta HEAD packed-refs index)
      if(EXISTS "${GIT_DIR_PATH}/${_git_meta}")
        list(APPEND GIT_METADATA_DEPENDS "${GIT_DIR_PATH}/${_git_meta}")
      endif()
    endforeach()
    if(GIT_HEAD_REF AND EXISTS "${GIT_DIR_PATH}/${GIT_HEAD_REF}")
      list(APPEND GIT_METADATA_DEPENDS "${GIT_DIR_PATH}/${GIT_HEAD_REF}")
    endif()
    if(GIT_METADATA_DEPENDS)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GIT_METADATA_DEPENDS})
      message(STATUS "Tracking Git HEAD for automatic version metadata refresh")
    endif()
  endif()

  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_HASH_FULL OUTPUT_STRIP_TRAILING_WHITESPACE)

  # Build provenance: the origin remote slug ("Owner/repo") is what proves a
  # binary was built from the canonical repository and not a stale, differently
  # named checkout (e.g. an old "Dinero-Coin" tree). A deploy gate can assert on
  # it; never hardcode the repo name.
  execute_process(COMMAND ${GIT_EXECUTABLE} remote get-url origin
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_REMOTE_URL OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
else()
  set(GIT_HASH "unknown")
  set(GIT_HASH_FULL "unknown")
  set(GIT_REMOTE_URL "")
  set(GIT_DESCRIBE "unknown")
endif()

# Normalize the remote URL to an "Owner/repo" slug, accepting both
# https://github.com/Owner/repo(.git) and git@github.com:Owner/repo(.git).
set(GIT_REMOTE_SLUG "${GIT_REMOTE_URL}")
if(GIT_REMOTE_SLUG)
  string(REGEX REPLACE "\\.git$" "" GIT_REMOTE_SLUG "${GIT_REMOTE_SLUG}")
  string(REGEX REPLACE "^.*[/:]([^/:]+/[^/:]+)$" "\\1" GIT_REMOTE_SLUG "${GIT_REMOTE_SLUG}")
endif()
if(NOT GIT_REMOTE_SLUG)
  set(GIT_REMOTE_SLUG "unknown")
endif()
if(NOT GIT_DESCRIBE)
  set(GIT_DESCRIBE "${GIT_HASH}")
endif()
message(STATUS "Build provenance: repo=${GIT_REMOTE_SLUG} describe=${GIT_DESCRIBE}")

# PROJECT_VERSION is the canonical semantic version set by project().
# GIT_HASH and GIT_HASH_FULL remain separate build-provenance values.
set(CLIENT_VERSION_MAJOR 1)
set(CLIENT_VERSION_MINOR 0)
set(CLIENT_VERSION_REVISION 0)
set(CLIENT_VERSION_BUILD 0)
set(CLIENT_VERSION_IS_RELEASE 0)
set(BUILD_DATE "${BUILD_TIME}")
set(GIT_COMMIT_ID "${GIT_HASH}")
set(GIT_COMMIT_FULL "${GIT_HASH_FULL}")

configure_file(${CMAKE_SOURCE_DIR}/include/compat/version_config.h.in
               ${CMAKE_BINARY_DIR}/version_config.h @ONLY)

include_directories(${CMAKE_BINARY_DIR})
