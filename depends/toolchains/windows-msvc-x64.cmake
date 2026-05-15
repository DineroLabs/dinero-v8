set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

# Run CMake from a Visual Studio Developer shell so cl.exe/link.exe are already
# on PATH. Ninja is preferred for deterministic native-MSVC dependency builds:
#   cmake -S depends -B depends/build-msvc -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=depends/toolchains/windows-msvc-x64.cmake
set(CMAKE_C_COMPILER cl.exe CACHE FILEPATH "C compiler")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Static CRT")
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")

add_compile_definitions(
  _WIN32_WINNT=0x0A00
  WIN32_LEAN_AND_MEAN
  NOMINMAX)

add_compile_options($<$<COMPILE_LANG_AND_ID:C,MSVC>:/W3>)
add_compile_options($<$<COMPILE_LANG_AND_ID:C,MSVC>:/O2>)
add_compile_options($<$<COMPILE_LANG_AND_ID:C,MSVC>:/Brepro>)

add_link_options($<$<C_COMPILER_ID:MSVC>:/Brepro>)
