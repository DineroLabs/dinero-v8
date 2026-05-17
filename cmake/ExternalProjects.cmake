# External sibling project discovery.
#
# Included from the repository root so cache paths and directory-scope include
# directories preserve the legacy behavior.

# dinero-qt is now a standalone project at ../dinero-qt/.
# Build it separately:
#   cd ../dinero-qt
#   cmake -B build -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"
#   cmake --build build
#
# Why separate?
#   - GUI bugs != consensus bugs
#   - GUI crashes must not kill the node
#   - GUI updates must not require consensus rebuilds
#   - GUI attack surface is huge and unrelated to validation
set(DINERO_QT_PATH "${CMAKE_SOURCE_DIR}/../dinero-qt" CACHE PATH "Path to dinero-qt project")
if(EXISTS "${DINERO_QT_PATH}/CMakeLists.txt")
  message(STATUS "dinero-qt project found at: ${DINERO_QT_PATH}")
  message(STATUS "  Build separately: cd ${DINERO_QT_PATH} && cmake -B build && cmake --build build")
else()
  message(STATUS "dinero-qt project not found at ${DINERO_QT_PATH}")
endif()

# Stratum is now a standalone project at ../stratum/.
# Build it separately: cd ../stratum && cmake -B build && cmake --build build
# The embedded stratum sources are still included in dinerod for integrated mode.
set(STRATUM_PROJECT_PATH "${CMAKE_SOURCE_DIR}/../stratum" CACHE PATH "Path to stratum project")
if(EXISTS "${STRATUM_PROJECT_PATH}/include")
  include_directories(${STRATUM_PROJECT_PATH}/include)
  message(STATUS "Stratum project found at: ${STRATUM_PROJECT_PATH}")
else()
  message(WARNING "Stratum project not found at ${STRATUM_PROJECT_PATH}")
endif()
