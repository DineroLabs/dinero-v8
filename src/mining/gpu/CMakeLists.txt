# GPU mining library (always built with stub implementations)
# Build unconditionally so daemon can link successfully even without GPU hardware
message(STATUS "Building GPU mining library:")

set(GPU_SOURCES
  src/mining/gpu/compute_backend.cpp
  src/mining/gpu/gpu_device_manager.cpp
)

if(ENABLE_GPU_MINING)
  message(STATUS "GPU mining support enabled - detecting backends...")

  # OpenCL detection (AMD, Intel, NVIDIA GPUs)
  if(APPLE)
    # macOS: System OpenCL framework
    find_library(OPENCL_LIBRARY OpenCL)
    if(OPENCL_LIBRARY)
      set(OPENCL_FOUND TRUE)
      add_library(OpenCL::OpenCL INTERFACE IMPORTED)
      set_target_properties(OpenCL::OpenCL PROPERTIES
        INTERFACE_LINK_LIBRARIES "${OPENCL_LIBRARY}"
      )
      message(STATUS "  ✅ OpenCL found (macOS system framework)")
    else()
      message(STATUS "  ❌ OpenCL not found")
    endif()
  elseif(WIN32)
    # Windows: probe OPENCL_ROOT or vendor-SDK env vars for Khronos headers
    # + an import library. On the canonical rc1 Windows build host the SDK
    # lives at C:/Users/<user>/openclsdk with Khronos headers under include/
    # and libOpenCL.dll.a (generated from C:\Windows\System32\OpenCL.dll via
    # gendef + dlltool) under lib/x64/. OPENCL_ROOT must point at that root.
    find_path(OPENCL_INCLUDE_DIR NAMES CL/cl.h
      HINTS ENV OPENCL_ROOT ENV OCL_ROOT ENV INTELOCLSDKROOT ENV AMDAPPSDKROOT ENV CUDA_PATH
      PATH_SUFFIXES include
    )
    find_library(OPENCL_LIBRARY NAMES OpenCL
      HINTS ENV OPENCL_ROOT ENV OCL_ROOT ENV INTELOCLSDKROOT ENV AMDAPPSDKROOT ENV CUDA_PATH
      PATH_SUFFIXES lib/x64 lib/x86_64 lib
    )
    if(OPENCL_INCLUDE_DIR AND OPENCL_LIBRARY)
      set(OPENCL_FOUND TRUE)
      add_library(OpenCL::OpenCL INTERFACE IMPORTED)
      set_target_properties(OpenCL::OpenCL PROPERTIES
        INTERFACE_LINK_LIBRARIES "${OPENCL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENCL_INCLUDE_DIR}"
      )
      message(STATUS "  ✅ OpenCL found (Windows: ${OPENCL_LIBRARY})")
    else()
      message(STATUS "  ❌ OpenCL not found on Windows (set OPENCL_ROOT to a")
      message(STATUS "     directory containing include/CL/*.h and lib/x64/libOpenCL.dll.a,")
      message(STATUS "     or install a vendor SDK that exports INTELOCLSDKROOT /")
      message(STATUS "     AMDAPPSDKROOT / CUDA_PATH)")
    endif()
  else()
    # Linux: Find OpenCL package
    find_package(OpenCL QUIET)
    if(OpenCL_FOUND)
      message(STATUS "  ✅ OpenCL found (version ${OpenCL_VERSION_STRING})")
    else()
      message(STATUS "  ❌ OpenCL not found (install ocl-icd-opencl-dev on Ubuntu/Debian)")
    endif()
  endif()

  # CUDA detection (NVIDIA GPUs only).
  # We do NOT call enable_language(CUDA) — the daemon's CUDA backend uses
  # NVRTC at runtime (same pattern as miner/src/gpu/cuda_backend.cpp), so no
  # nvcc invocation is required at build time. This sidesteps the CUDA 12.2
  # + MSVC 14.44 nvcc STL static_assert mismatch documented as Bug #4 in
  # NATIVE-MSVC-PORT-BUGS.md.
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND)
    message(STATUS "  ✅ CUDA Toolkit found (version ${CUDAToolkit_VERSION}) -- NVRTC path, no nvcc")
  else()
    message(STATUS "  ❌ CUDA Toolkit not found (NVIDIA GPU mining disabled)")
  endif()

  # Add OpenCL backend if available
  if(OPENCL_FOUND)
    list(APPEND GPU_SOURCES src/mining/gpu/opencl_backend.cpp)
    message(STATUS "  ✅ OpenCL backend (AMD/Intel/NVIDIA)")
  else()
    message(STATUS "  ❌ OpenCL backend (not available)")
  endif()

  # Add CUDA backend if available. The .cu kernel is NOT compiled by nvcc;
  # instead it's read at configure time from miner/shaders/sha256d.cu (the
  # single source of truth for the SHA-256d mining kernel, shared with
  # solo-miner) and embedded into the binary as a C-string literal by the
  # sha256d_cuda_src.cpp.in template. NVRTC compiles it at runtime against
  # the running GPU's exact compute capability.
  if(CUDAToolkit_FOUND)
    file(READ ${CMAKE_SOURCE_DIR}/miner/shaders/sha256d.cu DINERO_CUDA_KERNEL_SRC)
    configure_file(
      ${CMAKE_SOURCE_DIR}/src/mining/gpu/sha256d_cuda_src.cpp.in
      ${CMAKE_BINARY_DIR}/generated/sha256d_cuda_src.cpp
      @ONLY
    )
    list(APPEND GPU_SOURCES src/mining/gpu/cuda_backend.cpp)
    list(APPEND GPU_SOURCES ${CMAKE_BINARY_DIR}/generated/sha256d_cuda_src.cpp)
    message(STATUS "  ✅ CUDA backend (NVRTC; kernel embedded from miner/shaders/sha256d.cu)")
  else()
    message(STATUS "  ❌ CUDA backend (not available)")
  endif()

  # Metal detection (Apple Silicon — macOS native GPU API)
  if(APPLE)
    find_library(METAL_LIBRARY Metal)
    find_library(FOUNDATION_LIBRARY Foundation)
    if(METAL_LIBRARY AND FOUNDATION_LIBRARY)
      set(METAL_FOUND TRUE)
      list(APPEND GPU_SOURCES src/mining/gpu/metal_backend.mm)
      message(STATUS "  ✅ Metal backend (Apple Silicon native)")
    else()
      message(STATUS "  ❌ Metal not found")
    endif()
  endif()

  if(NOT OPENCL_FOUND AND NOT CUDAToolkit_FOUND AND NOT METAL_FOUND)
    message(STATUS "  ⚙️  Stub-only mode (no GPU hardware support)")
  endif()
else()
  # Even without ENABLE_GPU_MINING, auto-detect Metal on macOS
  # Metal is always available on Apple Silicon — enable it by default
  if(APPLE)
    find_library(METAL_LIBRARY Metal)
    find_library(FOUNDATION_LIBRARY Foundation)
    if(METAL_LIBRARY AND FOUNDATION_LIBRARY)
      set(METAL_FOUND TRUE)
      list(APPEND GPU_SOURCES src/mining/gpu/metal_backend.mm)
      message(STATUS "  ✅ Metal backend auto-detected (Apple Silicon)")
    endif()
  endif()
  if(NOT METAL_FOUND)
    message(STATUS "GPU mining support disabled (use -DENABLE_GPU_MINING=ON to enable)")
    message(STATUS "  ⚙️  Stub-only mode (no GPU hardware support)")
  endif()
endif()

add_library(dinero_gpu_mining STATIC ${GPU_SOURCES})

target_include_directories(dinero_gpu_mining PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# Link GPU libraries (only if backends were detected)
if(ENABLE_GPU_MINING)
  if(OPENCL_FOUND)
    target_compile_definitions(dinero_gpu_mining PUBLIC ENABLE_OPENCL)
    target_link_libraries(dinero_gpu_mining PUBLIC OpenCL::OpenCL)
  endif()

  if(CUDAToolkit_FOUND)
    target_compile_definitions(dinero_gpu_mining PUBLIC ENABLE_CUDA)
    # CUDA Driver API (cuInit/cuDeviceGet/cuMemAlloc/cuLaunchKernel) + NVRTC
    # (nvrtcCreateProgram/nvrtcCompileProgram/nvrtcGetPTX). No cudart — we
    # don't use the Runtime API in this backend (it's the nvcc-coupled path
    # that hits Bug #4 on MSVC native). NVRTC compiles the kernel at runtime
    # against the actual device's compute capability, so per-arch PTX
    # snapshots (CUDA_ARCHITECTURES) are no longer needed.
    target_link_libraries(dinero_gpu_mining PUBLIC
      CUDA::cuda_driver
      CUDA::nvrtc
    )
  endif()
endif()

# Metal backend (Apple Silicon) — linked regardless of ENABLE_GPU_MINING
if(METAL_FOUND)
  target_compile_definitions(dinero_gpu_mining PUBLIC ENABLE_METAL)
  target_link_libraries(dinero_gpu_mining PUBLIC "${METAL_LIBRARY}" "${FOUNDATION_LIBRARY}")
  # .mm files need Obj-C++ compilation
  set_source_files_properties(src/mining/gpu/metal_backend.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc"
  )
endif()

message(STATUS "  ✅ GPU mining library configured")
