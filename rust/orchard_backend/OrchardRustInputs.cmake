# Track nested wallet modules and every checked-in vendored dependency input.
# CONFIGURE_DEPENDS notices additions/removals; the manifest also invalidates
# the archive when a removed input no longer appears in the dependency list.
file(GLOB_RECURSE ORCHARD_RUST_INPUT_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${CMAKE_CURRENT_LIST_DIR}/src/*.rs"
  "${CMAKE_CURRENT_LIST_DIR}/vendor/*")
list(SORT ORCHARD_RUST_INPUT_FILES)
string(REPLACE ";" "\n" ORCHARD_RUST_INPUT_CONTENT "${ORCHARD_RUST_INPUT_FILES}")
set(ORCHARD_RUST_INPUT_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/orchard-rust-inputs.txt")
file(CONFIGURE OUTPUT "${ORCHARD_RUST_INPUT_MANIFEST}"
  CONTENT "${ORCHARD_RUST_INPUT_CONTENT}\n" @ONLY)
set(ORCHARD_RUST_INPUTS ${ORCHARD_RUST_INPUT_FILES}
  "${ORCHARD_RUST_INPUT_MANIFEST}" "${CMAKE_CURRENT_LIST_FILE}")
