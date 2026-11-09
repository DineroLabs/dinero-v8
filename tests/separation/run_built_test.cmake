# ctest driver for a build-driving separation test: build the
# (EXCLUDE_FROM_ALL) target, then run it with SEP_ARGS. A failure at either
# step fails the test.
foreach(v SEP_BUILD_DIR SEP_TARGET SEP_EXE)
  if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
    message(FATAL_ERROR "run_built_test.cmake: ${v} not set")
  endif()
endforeach()

set(_config_args)
if(DEFINED SEP_CONFIG AND NOT SEP_CONFIG STREQUAL "")
  set(_config_args --config ${SEP_CONFIG})
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} --build ${SEP_BUILD_DIR} --target ${SEP_TARGET} ${_config_args}
  RESULT_VARIABLE _build_rc)
if(NOT _build_rc EQUAL 0)
  message(FATAL_ERROR "SEPARATION TEST BUILD FAILED: ${SEP_TARGET} (rc=${_build_rc})")
endif()

execute_process(COMMAND ${SEP_EXE} ${SEP_ARGS} RESULT_VARIABLE _run_rc)
if(NOT _run_rc EQUAL 0)
  message(FATAL_ERROR "SEPARATION TEST FAILED: ${SEP_EXE} (rc=${_run_rc})")
endif()
message(STATUS "SEPARATION TEST OK: ${SEP_TARGET}")
