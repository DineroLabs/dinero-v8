# Runs the vector dumper and captures stdout to a file.
# A separate script because add_test() cannot redirect output portably.
execute_process(
  COMMAND "${EXE}"
  OUTPUT_FILE "${OUT}"
  RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "dump_shielded_vectors failed with ${rc}")
endif()
