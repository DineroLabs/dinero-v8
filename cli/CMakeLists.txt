cmake_minimum_required(VERSION 3.16)
project(DineroCLI VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CLI Core Library - single source of truth
add_library(dinero_cli_core STATIC
    src/net/url_parser.cpp
    src/commands/doctor.cpp
    src/commands/version.cpp
)

target_include_directories(dinero_cli_core PUBLIC
    include
)

target_compile_features(dinero_cli_core PUBLIC cxx_std_20)

# Get git information for build metadata
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)
    
    target_compile_definitions(dinero_cli_core PRIVATE
        GIT_SHA="${GIT_SHA}"
        GIT_BRANCH="${GIT_BRANCH}"
        BUILD_TIMESTAMP="${BUILD_TIMESTAMP}"
        CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    )
endif()

# Strict compilation flags to catch missing declarations
target_compile_options(dinero_cli_core PRIVATE
    -Werror
    -Wmissing-declarations
    -Wall
    -Wextra
)

message(STATUS "  ✅ dinero_cli_core - URL parser library (single source of truth)")
