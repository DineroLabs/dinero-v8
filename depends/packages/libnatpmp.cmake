set(libnatpmp_version 20230423)
set(libnatpmp_file "libnatpmp-${libnatpmp_version}.tar.gz")
set(libnatpmp_url "https://miniupnp.tuxfamily.org/files/${libnatpmp_file}")
set(libnatpmp_sha256 0684ed2c8406437e7519a1bd20ea83780db871b3a3a5d752311ba3e889dbfc70)

ExternalProject_Add(libnatpmp
  URL "${libnatpmp_url}"
  URL_HASH SHA256=${libnatpmp_sha256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  DOWNLOAD_DIR "${DINERO_DEPENDS_SOURCES_DIR}"
  PREFIX "${DINERO_DEPENDS_WORK_DIR}/libnatpmp"
  INSTALL_DIR "${DINERO_DEPENDS_PREFIX}"
  CONFIGURE_COMMAND
    ${CMAKE_COMMAND}
    -S "${DINERO_DEPENDS_DIR}/cmake/libnatpmp"
    -B <BINARY_DIR>
    -G "${CMAKE_GENERATOR}"
    ${DINERO_DEPENDS_COMMON_CMAKE_ARGS}
    -DLIBNATPMP_SOURCE_DIR=<SOURCE_DIR>
  BUILD_COMMAND
    ${CMAKE_COMMAND} --build <BINARY_DIR>
  INSTALL_COMMAND
    ${CMAKE_COMMAND} --install <BINARY_DIR>)
