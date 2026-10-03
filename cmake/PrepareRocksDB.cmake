# Keep the pinned upstream checkout immutable. Only the checksum source in
# this build's private RocksDB source copy is replaced, after an exact-source
# guard. An upstream revision requires explicit review of this carried fix.
function(dinero_prepare_rocksdb_source upstream output)
  file(SHA256 "${upstream}/util/crc32c_arm64.cc" checksum_source_sha)
  if(NOT checksum_source_sha STREQUAL "00a22ab1efa7a009112ef57c67fc8b3918af1f9e964041e2ccf91e1eca926fcb")
    message(FATAL_ERROR "RocksDB ARM checksum source changed; review the carried byte-load fix")
  endif()
  file(MAKE_DIRECTORY "${output}")
  file(COPY "${upstream}/" DESTINATION "${output}" PATTERN ".git" EXCLUDE)
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/rocksdb/crc32c_arm64.cc"
                 "${output}/util/crc32c_arm64.cc" COPYONLY)
endfunction()
