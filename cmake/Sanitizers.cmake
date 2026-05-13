# Sanitizers.cmake - Consistent sanitizer configuration for DineroCoin
# This ensures all executables and tests link with the same sanitizer runtime
# as the libraries they depend on.

# ThreadSanitizer option (mutually exclusive with ASan/UBSan)
option(ENABLE_TSAN "Enable ThreadSanitizer (mutually exclusive with ASan/UBSan)" OFF)

# Create an interface target that propagates sanitizer flags
add_library(dinero_sanitizers INTERFACE)

if(DIN_ENABLE_ASAN)
  if(ENABLE_TSAN)
    # ThreadSanitizer mode
    target_compile_options(dinero_sanitizers INTERFACE 
        -fno-omit-frame-pointer 
        -fsanitize=thread
    )
    target_link_options(dinero_sanitizers INTERFACE 
        -fsanitize=thread
    )
    message(STATUS "ThreadSanitizer enabled (TSAN mode)")
  else()
    # AddressSanitizer + UndefinedBehaviorSanitizer mode (default)
    target_compile_options(dinero_sanitizers INTERFACE 
        -fno-omit-frame-pointer 
        -fsanitize=address,undefined
    )
    target_link_options(dinero_sanitizers INTERFACE 
        -fsanitize=address,undefined
    )
    message(STATUS "AddressSanitizer + UndefinedBehaviorSanitizer enabled (ASan+UBSan mode)")
  endif()
else()
  message(STATUS "Sanitizers disabled")
endif()

# Helper function to link sanitizers to targets
function(dinero_enable_sanitizers target_name)
    target_link_libraries(${target_name} PRIVATE dinero_sanitizers)
endfunction()

# Note: Any executable or test that links libraries compiled with sanitizers
# MUST also link dinero_sanitizers to pull in the sanitizer runtime.
# 
# Usage:
#   target_link_libraries(my_executable PRIVATE dinero_common dinero_sanitizers)
#   # OR use the helper:
#   dinero_enable_sanitizers(my_executable)