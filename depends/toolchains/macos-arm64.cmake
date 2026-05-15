set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)

set(CMAKE_C_COMPILER /usr/bin/clang CACHE FILEPATH "C compiler")
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "macOS architecture")
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "Minimum macOS version")

execute_process(
  COMMAND xcrun --sdk macosx --show-sdk-path
  OUTPUT_VARIABLE _dinero_macos_sdk
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(_dinero_macos_sdk)
  set(CMAKE_OSX_SYSROOT "${_dinero_macos_sdk}" CACHE PATH "macOS SDK")
endif()
