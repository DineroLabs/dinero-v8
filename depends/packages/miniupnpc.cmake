set(miniupnpc_version 2.3.3)
set(miniupnpc_file "miniupnpc-${miniupnpc_version}.tar.gz")
set(miniupnpc_url "https://miniupnp.tuxfamily.org/files/${miniupnpc_file}")
set(miniupnpc_sha256 d52a0afa614ad6c088cc9ddff1ae7d29c8c595ac5fdd321170a05f41e634bd1a)

ExternalProject_Add(miniupnpc
  URL "${miniupnpc_url}"
  URL_HASH SHA256=${miniupnpc_sha256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  DOWNLOAD_DIR "${DINERO_DEPENDS_SOURCES_DIR}"
  PREFIX "${DINERO_DEPENDS_WORK_DIR}/miniupnpc"
  INSTALL_DIR "${DINERO_DEPENDS_PREFIX}"
  CONFIGURE_COMMAND
    ${CMAKE_COMMAND}
    -S "${DINERO_DEPENDS_DIR}/cmake/miniupnpc"
    -B <BINARY_DIR>
    -G "${CMAKE_GENERATOR}"
    ${DINERO_DEPENDS_COMMON_CMAKE_ARGS}
    -DMINIUPNPC_SOURCE_DIR=<SOURCE_DIR>
  BUILD_COMMAND
    ${CMAKE_COMMAND} --build <BINARY_DIR>
  INSTALL_COMMAND
    ${CMAKE_COMMAND} --install <BINARY_DIR>)
