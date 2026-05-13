# Reproducible Build Configuration for Dinero
# Ensures bit-for-bit identical builds across different machines and environments

# Set deterministic build flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Enabling reproducible build flags for Release build")
    
    # Use LLVM tools for deterministic archives if available
    find_program(LLVM_AR llvm-ar)
    find_program(LLVM_RANLIB llvm-ranlib)
    
    if(LLVM_AR AND LLVM_RANLIB)
        set(CMAKE_AR "${LLVM_AR}" CACHE FILEPATH "Archiver for reproducible builds")
        set(CMAKE_RANLIB "${LLVM_RANLIB}" CACHE FILEPATH "Ranlib for reproducible builds")
        message(STATUS "Using LLVM tools for reproducible archives: ${LLVM_AR}, ${LLVM_RANLIB}")
    else()
        message(STATUS "LLVM tools not found, using system ar/ranlib (may affect reproducibility)")
    endif()
    
    # Deterministic compilation flags
    add_compile_options(
        # Remove absolute paths from debug info and __FILE__ macros
        -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
        -fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.
    )
    
    # Deterministic linking flags
    if(NOT WIN32)
        add_link_options(
            # Remove build-id for deterministic binaries
            -Wl,--build-id=none
        )
    endif()
    
    # Set SOURCE_DATE_EPOCH for deterministic timestamps
    if(NOT DEFINED ENV{SOURCE_DATE_EPOCH})
        # Use git commit timestamp if available, otherwise use a fixed date
        execute_process(
            COMMAND git log -1 --format=%ct
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_TIMESTAMP
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        
        if(GIT_COMMIT_TIMESTAMP)
            set(ENV{SOURCE_DATE_EPOCH} "${GIT_COMMIT_TIMESTAMP}")
            message(STATUS "Using git commit timestamp for SOURCE_DATE_EPOCH: ${GIT_COMMIT_TIMESTAMP}")
        else()
            # Fallback to a fixed date (Dinero project start)
            set(ENV{SOURCE_DATE_EPOCH} "1692576000")  # August 20, 2023
            message(STATUS "Using fallback SOURCE_DATE_EPOCH: $ENV{SOURCE_DATE_EPOCH}")
        endif()
    else()
        message(STATUS "Using provided SOURCE_DATE_EPOCH: $ENV{SOURCE_DATE_EPOCH}")
    endif()
    
    # Ensure deterministic archives
    set(ENV{ARFLAGS} "crD")
    
    # Set deterministic C++ standard library behavior
    add_compile_definitions(
        # Disable non-deterministic features
        _FORTIFY_SOURCE=0
    )
    
    # Platform-specific reproducible build settings
    if(WIN32)
        # Windows-specific deterministic flags
        add_compile_options(
            /Brepro  # Enable reproducible builds in MSVC
        )
        add_link_options(
            /Brepro  # Enable reproducible linking in MSVC
        )
    endif()
    
    if(APPLE)
        # macOS-specific deterministic flags
        add_link_options(
            -Wl,-no_uuid  # Remove UUID from Mach-O header
        )
    endif()
    
endif()

# Function to verify reproducible build environment
function(verify_reproducible_environment)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(STATUS "=== Reproducible Build Environment ===")
        message(STATUS "CMAKE_AR: ${CMAKE_AR}")
        message(STATUS "CMAKE_RANLIB: ${CMAKE_RANLIB}")
        message(STATUS "SOURCE_DATE_EPOCH: $ENV{SOURCE_DATE_EPOCH}")
        message(STATUS "ARFLAGS: $ENV{ARFLAGS}")
        message(STATUS "Build timestamp will be: $ENV{SOURCE_DATE_EPOCH}")
        
        # Verify critical tools are available
        if(NOT CMAKE_AR OR NOT CMAKE_RANLIB)
            message(WARNING "Standard ar/ranlib in use - reproducibility may be affected")
        endif()
        
        message(STATUS "=====================================")
    endif()
endfunction()

# Call verification function
verify_reproducible_environment()
