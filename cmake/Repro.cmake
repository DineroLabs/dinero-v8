# Cross-Platform Reproducible Build Configuration for Dinero
# Ensures bit-for-bit identical builds across Windows, macOS, and Linux

message(STATUS "Configuring reproducible build settings...")

# Set deterministic timestamp (can be overridden by environment)
if(NOT DEFINED ENV{SOURCE_DATE_EPOCH})
    set(ENV{SOURCE_DATE_EPOCH} "1692576000")  # August 20, 2023 - Dinero project start
endif()

message(STATUS "Using SOURCE_DATE_EPOCH: $ENV{SOURCE_DATE_EPOCH}")

# Cross-platform reproducible compilation flags
add_compile_options(
    # Remove absolute paths from debug info and __FILE__ macros
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.
)

# Don't embed linker build-ids on ELF (breaks byte-for-byte reproducibility)
if(UNIX AND NOT APPLE)
    add_link_options(-Wl,--build-id=none)
    message(STATUS "Disabled ELF build-id for reproducible Linux builds")
endif()

# Deterministic archives on Unix systems
if(UNIX)
    # Use deterministic archive creation (remove D flag for llvm-ar compatibility)
    set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_C_ARCHIVE_FINISH   "<CMAKE_RANLIB> <TARGET>")
    set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> <TARGET>")
    set(ENV{ARFLAGS} "cr")
    
    # Prefer LLVM tools for better reproducibility
    find_program(LLVM_AR llvm-ar)
    find_program(LLVM_RANLIB llvm-ranlib)
    
    if(LLVM_AR AND LLVM_RANLIB)
        set(CMAKE_AR "${LLVM_AR}" CACHE FILEPATH "LLVM archiver for reproducible builds")
        set(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "LLVM ranlib for reproducible builds")
        message(STATUS "Using LLVM tools for reproducible archives: ${LLVM_AR}, ${LLVM_RANLIB}")
    else()
        message(STATUS "LLVM tools not found, using system ar/ranlib")
        if(APPLE)
            # Try Xcode LLVM tools on macOS
            execute_process(
                COMMAND xcode-select -p
                OUTPUT_VARIABLE XCODE_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(XCODE_PATH)
                set(XCODE_LLVM_PATH "${XCODE_PATH}/Toolchains/XcodeDefault.xctoolchain/usr/bin")
                find_program(XCODE_LLVM_AR "${XCODE_LLVM_PATH}/llvm-ar")
                find_program(XCODE_LLVM_RANLIB "${XCODE_LLVM_PATH}/llvm-ranlib")
                
                if(XCODE_LLVM_AR AND XCODE_LLVM_RANLIB)
                    set(CMAKE_AR "${XCODE_LLVM_AR}" CACHE FILEPATH "Xcode LLVM archiver")
                    set(CMAKE_RANLIB "${XCODE_LLVM_RANLIB}" CACHE FILEPATH "Xcode LLVM ranlib")
                    message(STATUS "Using Xcode LLVM tools: ${XCODE_LLVM_AR}, ${XCODE_LLVM_RANLIB}")
                endif()
            endif()
        endif()
    endif()
endif()

# MSVC reproducibility settings + static CRT
if(MSVC)
    add_compile_options(
        /nologo         # Don't show compiler banner
        /W3             # Warning level 3
        /Z7             # Keep debug info in .obj for determinism (not .pdb)
    )
    add_link_options(
        /Brepro         # Enable reproducible .lib/.dll/.exe generation
    )
    
    # Force static CRT linking for fully self-contained binaries
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Static CRT")
    message(STATUS "MSVC: Enabled reproducible builds with static CRT (/MT)")
endif()

# Disable non-essential features that can affect reproducibility
option(DINERO_WITH_VULKAN "Enable Vulkan support" OFF)

# Additional hardening flags for Linux (optional)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_BUILD_TYPE STREQUAL "Release")
    option(DINERO_LINUX_HARDENING "Enable Linux security hardening flags" ON)
    if(DINERO_LINUX_HARDENING)
        add_compile_options(
            -fno-plt                    # Disable PLT for better security
            -D_FORTIFY_SOURCE=2         # Enable buffer overflow detection
        )
        add_link_options(
            -Wl,-z,relro,-z,now        # Enable RELRO and immediate binding
        )
        message(STATUS "Linux: Enabled security hardening flags")
    endif()
endif()

# Function to verify reproducible build environment
function(verify_repro_environment)
    message(STATUS "=== Reproducible Build Environment ===")
    message(STATUS "Platform: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
    message(STATUS "SOURCE_DATE_EPOCH: $ENV{SOURCE_DATE_EPOCH}")
    
    if(UNIX)
        message(STATUS "CMAKE_AR: ${CMAKE_AR}")
        message(STATUS "CMAKE_RANLIB: ${CMAKE_RANLIB}")
        message(STATUS "ARFLAGS: $ENV{ARFLAGS}")
    endif()
    
    if(MSVC)
        message(STATUS "MSVC Runtime: ${CMAKE_MSVC_RUNTIME_LIBRARY}")
    endif()
    
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        message(STATUS "macOS Architectures: ${CMAKE_OSX_ARCHITECTURES}")
    endif()
    
    message(STATUS "=====================================")
endfunction()

# Call verification
verify_repro_environment()

message(STATUS "Reproducible build configuration complete")
