# Hardware Wallet Security: Mock Signing Control
#
# Mock signing fabricates signatures for tests. Keep this default-off and
# reject production-like build types when it is explicitly enabled.

option(DIN_HW_WALLET_MOCK "Enable hardware wallet mock signing (TEST ONLY - Debug builds only)" OFF)

if(DIN_HW_WALLET_MOCK)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    message(FATAL_ERROR
      "\n"
      "═══════════════════════════════════════════════════════════════════════════\n"
      "FATAL: DIN_HW_WALLET_MOCK=ON is not allowed with CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}\n"
      "═══════════════════════════════════════════════════════════════════════════\n"
      "\n"
      "Mock signing returns FAKE SIGNATURES that would broadcast invalid transactions.\n"
      "This flag is only allowed in Debug builds for testing.\n"
      "\n"
      "To fix:\n"
      "  - For production: Remove -DDIN_HW_WALLET_MOCK=ON\n"
      "  - For testing: Use -DCMAKE_BUILD_TYPE=Debug\n"
      "\n"
    )
  endif()

  add_compile_definitions(DIN_HW_WALLET_MOCK)
  message(WARNING "")
  message(WARNING "═══════════════════════════════════════════════════════════════════════════")
  message(WARNING "⚠️  HARDWARE WALLET MOCK SIGNING ENABLED - NOT PRODUCTION SAFE")
  message(WARNING "═══════════════════════════════════════════════════════════════════════════")
  message(WARNING "⚠️  This build will fabricate signatures for testing")
  message(WARNING "⚠️  DO NOT use this build for real funds")
  message(WARNING "═══════════════════════════════════════════════════════════════════════════")
  message(WARNING "")
else()
  message(STATUS "✅ Hardware wallet mock signing DISABLED (production-safe)")
endif()
