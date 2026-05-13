# SQLite3 Amalgamation Build
# Note: Do not call cmake_minimum_required() in subdirectory as it interferes with parent

# Create static library from amalgamation
add_library(sqlite3 STATIC sqlite3.c)

# Output library to build/lib directory
set_target_properties(sqlite3 PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Public headers
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Compiler flags for SQLite
if(UNIX)
    target_compile_definitions(sqlite3 PRIVATE
        -DSQLITE_ENABLE_COLUMN_METADATA
        -DSQLITE_ENABLE_FTS5
        -DSQLITE_ENABLE_JSON1
        -DSQLITE_ENABLE_RTREE
        -DSQLITE_THREADSAFE=1
    )
    # Disable warnings for vendored code
    target_compile_options(sqlite3 PRIVATE -w)
endif()

if(WIN32)
    target_compile_definitions(sqlite3 PRIVATE
        -DSQLITE_ENABLE_COLUMN_METADATA
        -DSQLITE_ENABLE_FTS5
        -DSQLITE_ENABLE_JSON1
        -DSQLITE_ENABLE_RTREE
        -DSQLITE_THREADSAFE=1
    )
endif()
