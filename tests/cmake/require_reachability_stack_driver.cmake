# Script-mode driver for scripts/ci/test_require_reachability_stack.py:
#   cmake -D<VAR>=<VALUE>... -P tests/cmake/require_reachability_stack_driver.cmake
# Runs the release gate against whatever feature state the caller passes in,
# without configuring the whole project.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/RequireReachabilityStack.cmake)
dinero_require_reachability_stack()
