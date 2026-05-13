# Static SQLite Integration for Dinero
# 
# This module provides static SQLite linking to avoid Homebrew dependencies
# and ensure consistent behavior across all platforms.

# SQLite source directory
set(SQLITE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")

# Check if SQLite source exists
if(NOT EXISTS "${SQLITE_SOURCE_DIR}/sqlite3.c")
    message(FATAL_ERROR "SQLite source not found. Please run: cd third_party/sqlite && curl -O https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip && unzip sqlite-amalgamation-3450100.zip && mv sqlite-amalgamation-3450100/* . && rmdir sqlite-amalgamation-3450100")
endif()

# Create static SQLite library
add_library(sqlite3 STATIC "${SQLITE_SOURCE_DIR}/sqlite3.c")

# Set the language for SQLite (it's C, not C++)
set_target_properties(sqlite3 PROPERTIES LINKER_LANGUAGE C)

# Set include directories
target_include_directories(sqlite3 
    PUBLIC "${SQLITE_SOURCE_DIR}"
    PRIVATE "${SQLITE_SOURCE_DIR}"
)

# Set compile definitions for optimal SQLite configuration
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1          # Thread-safe mode
    SQLITE_OMIT_LOAD_EXTENSION=1 # No extension loading (security)
    SQLITE_ENABLE_JSON1=1        # Enable JSON functions
    SQLITE_ENABLE_DBSTAT_VTAB=1  # Enable database statistics
    SQLITE_ENABLE_FTS5=1         # Enable full-text search
    SQLITE_ENABLE_RTREE=1        # Enable R-tree extension
    SQLITE_ENABLE_GEOPOLY=1      # Enable geopoly extension
    SQLITE_ENABLE_MATH_FUNCTIONS=1 # Enable math functions
    SQLITE_ENABLE_UNKNOWN_SQL_FUNCTION=1 # Allow unknown functions
    SQLITE_ENABLE_STMTVTAB=1     # Enable statement virtual table
    SQLITE_ENABLE_DBPAGE_VTAB=1  # Enable database page virtual table
    SQLITE_ENABLE_SORTER_REFERENCES=1 # Enable sorter references
    SQLITE_ENABLE_OFFSET_SQL_FUNCTION=1 # Enable offset function
    SQLITE_ENABLE_EXPLAIN_COMMENTS=1 # Enable explain comments
)

# Set C standard
set_target_properties(sqlite3 PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# Platform-specific optimizations
if(APPLE)
    # macOS optimizations
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_ENABLE_LOCKING_STYLE=1  # Better locking on macOS
        SQLITE_ENABLE_HAVE_ISNAN=1     # Use system isnan
    )
elseif(WIN32)
    # Windows optimizations
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_WIN32_MALLOC=1          # Use Windows memory allocator
        SQLITE_WIN32_GETVERSIONEX=1    # Use Windows version API
    )
elseif(UNIX AND NOT APPLE)
    # Linux optimizations
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_ENABLE_LOCKING_STYLE=0  # Disable macOS-specific locking on Linux
        _GNU_SOURCE=1                  # Enable GNU extensions
        SQLITE_OMIT_FS_ATTRIBUTES=1    # Disable filesystem attributes (fixes MNT_LOCAL error)
    )
endif()

# Performance optimizations
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DEFAULT_CACHE_SIZE=10000    # Larger cache
    SQLITE_DEFAULT_PAGE_SIZE=4096      # 4KB pages
    SQLITE_DEFAULT_TEMP_CACHE_SIZE=1000 # Temp cache size
    SQLITE_MAX_EXPR_DEPTH=1000         # Expression depth
    SQLITE_MAX_VDBE_OP=250000          # VDBE operations
    SQLITE_MAX_FUNCTION_ARG=127        # Function arguments
    SQLITE_MAX_ATTACHED=125            # Attached databases
    SQLITE_MAX_LIKE_PATTERN_LENGTH=50000 # LIKE pattern length
    SQLITE_MAX_VARIABLE_NUMBER=999     # Variable number
    SQLITE_MAX_COLUMN=2000             # Maximum columns
    SQLITE_MAX_SQL_LENGTH=1000000      # SQL length
    SQLITE_MAX_LENGTH=1000000000       # Maximum length
    SQLITE_MAX_TRIGGER_DEPTH=1000      # Trigger depth
    SQLITE_MAX_INDEX=1000              # Maximum indexes
    SQLITE_MAX_TABLE=1000              # Maximum tables
    SQLITE_MAX_VIEW=1000               # Maximum views
    SQLITE_MAX_TRIGGER=1000            # Maximum triggers
    SQLITE_MAX_INDEX_EXPR=1000         # Index expressions
    SQLITE_MAX_INDEX_COLUMN=16         # Index columns
    SQLITE_MAX_TRIGGER_EXPR=1000       # Trigger expressions
    SQLITE_MAX_TRIGGER_COLUMN=16       # Trigger columns
)

# Security features
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_SECURE_DELETE=1             # Secure deletion
    SQLITE_ENABLE_OVERSIZE_CELL_CHECK=1 # Oversize cell check
    SQLITE_ENABLE_MEMORY_MANAGEMENT=1  # Memory management
    SQLITE_ENABLE_COLUMN_METADATA=1    # Column metadata
    SQLITE_ENABLE_UNLOCK_NOTIFY=1      # Unlock notifications
    SQLITE_ENABLE_PREUPDATE_HOOK=1     # Pre-update hooks
    SQLITE_ENABLE_SESSION=1            # Sessions
)

# Link with math library for math functions
if(UNIX AND NOT APPLE)
    target_link_libraries(sqlite3 PRIVATE m)
endif()

# Print SQLite configuration
message(STATUS "SQLite Configuration:")
message(STATUS "  Source: ${SQLITE_SOURCE_DIR}")
message(STATUS "  Thread-safe: Yes")
message(STATUS "  Extensions: Disabled (security)")
message(STATUS "  JSON: Enabled")
message(STATUS "  FTS: Enabled")
message(STATUS "  RTREE: Enabled")
message(STATUS "  Math: Enabled")
