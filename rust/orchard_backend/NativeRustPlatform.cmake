function(dinero_orchard_msvc_environment)
  foreach(flag_env RUSTFLAGS CARGO_ENCODED_RUSTFLAGS RUSTC RUSTC_WRAPPER
      RUSTC_WORKSPACE_WRAPPER CARGO_BUILD_RUSTC CARGO_BUILD_RUSTC_WRAPPER
      CARGO_BUILD_RUSTC_WORKSPACE_WRAPPER)
    if(DEFINED ENV{${flag_env}} AND NOT "$ENV{${flag_env}}" STREQUAL "")
      message(FATAL_ERROR "Uncontrolled ${flag_env} in Orchard MSVC qualification")
    endif()
  endforeach()
endfunction()

# Platform contract for the staged native Rust static library. This function is
# also exercised in CMake script mode without loading a compiler or running Cargo.
function(dinero_orchard_native_platform out_target out_archive)
  if(CMAKE_CROSSCOMPILING OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "Orchard backend cross/mobile toolchain is not qualified yet")
  endif()
  if(WIN32)
    if(NOT DINERO_ORCHARD_WINDOWS_QUALIFICATION)
      message(FATAL_ERROR "Orchard Windows release is not qualified; use the isolated qualification option only for testing")
    endif()
    if(NOT CMAKE_HOST_WIN32 OR NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
       NOT MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64" OR
       NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
      message(FATAL_ERROR "Orchard Windows qualification requires native x64 MSVC")
    endif()
    if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "" AND
       NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDLL" AND
       NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
      message(FATAL_ERROR "Orchard Windows qualification requires the dynamic MSVC runtime")
    endif()
    foreach(flags CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_C_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_C_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_MINSIZEREL)
      if("${${flags}}" MATCHES "(^|[ \t])[-/]M(Td?|Dd)([ \t]|$)")
        message(FATAL_ERROR "Conflicting MSVC runtime compiler flag in ${flags}")
      endif()
    endforeach()
    dinero_orchard_msvc_environment()
    set(${out_target} "x86_64-pc-windows-msvc" PARENT_SCOPE)
    set(${out_archive} "dinero_orchard_backend.lib" PARENT_SCOPE)
  else()
    if(APPLE AND CMAKE_OSX_ARCHITECTURES AND
       NOT CMAKE_OSX_ARCHITECTURES STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
      message(FATAL_ERROR "Orchard backend currently requires the native macOS architecture")
    endif()
    set(${out_target} "" PARENT_SCOPE)
    set(${out_archive} "libdinero_orchard_backend.a" PARENT_SCOPE)
  endif()
endfunction()
