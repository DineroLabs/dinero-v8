# ctest driver for a build-driving separation check that is a script: build
# the targets in SEP_TARGETS, then run SEP_COMMAND. Both are '|'-separated
# (a ';' would be split by add_test). A failure at either step fails the test.
foreach(v SEP_BUILD_DIR SEP_TARGETS SEP_COMMAND)
  if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
    message(FATAL_ERROR "run_built_script.cmake: ${v} not set")
  endif()
endforeach()
string(REPLACE "|" ";" _targets "${SEP_TARGETS}")
string(REPLACE "|" ";" _command "${SEP_COMMAND}")

set(_config_args)
if(DEFINED SEP_CONFIG AND NOT SEP_CONFIG STREQUAL "")
  set(_config_args --config ${SEP_CONFIG})
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} --build ${SEP_BUILD_DIR} --target ${_targets} ${_config_args}
  RESULT_VARIABLE _build_rc)
if(NOT _build_rc EQUAL 0)
  message(FATAL_ERROR "SEPARATION TEST BUILD FAILED: ${_targets} (rc=${_build_rc})")
endif()

execute_process(COMMAND ${_command} RESULT_VARIABLE _run_rc)
if(NOT _run_rc EQUAL 0)
  message(FATAL_ERROR "SEPARATION TEST FAILED: ${_command} (rc=${_run_rc})")
endif()
message(STATUS "SEPARATION TEST OK: ${_command}")
