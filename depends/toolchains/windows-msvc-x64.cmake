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

# Force CMake's try-compile probes (compiler detection, feature tests) to use
# the Release configuration. Without this, multi-config generators (Visual
# Studio, Xcode) default the try-compile to Debug — which on MSVC injects
# /RTC1 — and an unguarded /O2 below would collide with cl D8016
# "command-line options are incompatible". Ninja is single-config Release
# so this is a no-op there; for VS gen it makes the toolchain usable.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release CACHE STRING "")

add_compile_definitions(
  _WIN32_WINNT=0x0A00
  WIN32_LEAN_AND_MEAN
  NOMINMAX)

add_compile_options($<$<COMPILE_LANG_AND_ID:C,MSVC>:/W3>)
# Optimization flags are gated to Release so they never collide with the
# Debug-config /RTC1 runtime-checks flag CMake's try-compile may inject.
# CMake's built-in CMAKE_C_FLAGS_RELEASE already contains "/MD /O2 /Ob2
# /DNDEBUG" for MSVC, so this is largely belt-and-suspenders — kept explicit
# so a reader doesn't wonder whether depends are unoptimized.
add_compile_options($<$<AND:$<COMPILE_LANG_AND_ID:C,MSVC>,$<CONFIG:Release>>:/O2>)
# /Brepro is reproducibility metadata, applies in every config.
add_compile_options($<$<COMPILE_LANG_AND_ID:C,MSVC>:/Brepro>)

add_link_options($<$<C_COMPILER_ID:MSVC>:/Brepro>)
