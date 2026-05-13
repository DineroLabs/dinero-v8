# Dinero Utility Library
# Contains shared serialization and utility functions

# Source files for util library
set(UTIL_SOURCES
    ser.cpp
    args.cpp
    netselect.cpp
)

# Create util library
add_library(dinero_util STATIC ${UTIL_SOURCES})

# Set include directories
target_include_directories(dinero_util
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

# Compiler flags
target_compile_options(dinero_util PRIVATE
    -Wall -Wextra -O3
)

# Install util library
install(TARGETS dinero_util
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
