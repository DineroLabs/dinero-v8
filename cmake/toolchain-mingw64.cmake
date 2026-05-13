# Toolchain file for cross-compiling to Windows x86_64 from macOS using MinGW-w64

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Specify the cross compiler
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Where is the target environment located
set(CMAKE_FIND_ROOT_PATH /opt/homebrew/Cellar/mingw-w64/13.0.0_2/toolchain-x86_64)

# Adjust the default behavior of the FIND_XXX() commands:
# search programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# search headers and libraries in the target environment
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Windows-specific settings
set(WIN32 1)
set(MINGW 1)

# Static linking for Windows portability
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")

# Disable position independent code for Windows
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
