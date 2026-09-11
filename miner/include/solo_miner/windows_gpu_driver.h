#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dinero::gpu_driver {
// Driver modules are owned by the OS/vendor installation, not by the wallet.
// Keep a successfully loaded module alive while delayed imports reference it.
inline bool cudaAvailable() {
#if defined(_WIN32)
    static const HMODULE module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module != nullptr;
#else
    return true;
#endif
}
inline bool openclAvailable() {
#if defined(_WIN32)
    static const HMODULE module = LoadLibraryExW(L"OpenCL.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module != nullptr;
#else
    return true;
#endif
}
} // namespace dinero::gpu_driver
