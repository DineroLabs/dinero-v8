# cmake/BundleDeps.cmake
# Auto-bundle non-system dylibs into macOS app bundles (..../Frameworks) or next to CLI binaries.
# Uses CMake's BundleUtilities (fixup_bundle) + optional macdeployqt.

function(bundle_macos_shared_deps TARGET)
  if(NOT APPLE)
    message(STATUS "bundle_macos_shared_deps: non-Apple platform, skipping for ${TARGET}")
    return()
  endif()

  include(CMakeParseArguments)
  cmake_parse_arguments(BMSD "" "" "HINT_DIRS" ${ARGN})

  # Helpful rpaths
  get_target_property(_is_bundle ${TARGET} MACOSX_BUNDLE)
  if(_is_bundle)
    # App bundle looks in Contents/Frameworks
    set_property(TARGET ${TARGET} APPEND PROPERTY INSTALL_RPATH "@executable_path/../Frameworks")
    set_property(TARGET ${TARGET} PROPERTY BUILD_WITH_INSTALL_RPATH TRUE)
  else()
    # CLI looks next to the binary
    set_property(TARGET ${TARGET} APPEND PROPERTY INSTALL_RPATH "@executable_path")
    set_property(TARGET ${TARGET} PROPERTY BUILD_WITH_INSTALL_RPATH TRUE)
  endif()

  # Collect search dirs: user hints + Qt dirs if available  
  set(_hint_dirs "")
  if(BMSD_HINT_DIRS)
    list(APPEND _hint_dirs ${BMSD_HINT_DIRS})
  endif()
  
  # Add Qt library directories if Qt targets exist
  if(TARGET Qt6::Core)
    get_target_property(_qt6_core_location Qt6::Core LOCATION)
    if(_qt6_core_location)
      get_filename_component(_qt6_lib_dir "${_qt6_core_location}" DIRECTORY)
      list(APPEND _hint_dirs "${_qt6_lib_dir}")
    endif()
  endif()
  
  if(_hint_dirs)
    list(REMOVE_DUPLICATES _hint_dirs)
  endif()

  # For app bundles, use macdeployqt first to handle Qt frameworks properly
  if(_is_bundle)
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt)
    if(MACDEPLOYQT_EXECUTABLE)
      add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND "${MACDEPLOYQT_EXECUTABLE}" "$<TARGET_BUNDLE_DIR:${TARGET}>" -always-overwrite -verbose=2
        COMMENT "macdeployqt: deploying Qt frameworks for ${TARGET}" VERBATIM)
      
      # Add audit for app bundles to ensure no Homebrew paths remain
      add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND "${PROJECT_SOURCE_DIR}/cmake/audit_qt_bundle.sh" "$<TARGET_BUNDLE_DIR:${TARGET}>"
        COMMENT "Auditing ${TARGET}.app for Homebrew dependencies"
      )
    else()
      message(WARNING "macdeployqt not found - Qt app ${TARGET} may not be self-contained")
    endif()
  endif()

  # Convert hint dirs list to a string for passing to the script
  string(REPLACE ";" "," _hint_dirs_str "${_hint_dirs}")

  # For CLI binaries (non-bundles), use our custom bundling script
  if(NOT _is_bundle)
    add_custom_command(TARGET ${TARGET} POST_BUILD
      COMMAND "${PROJECT_SOURCE_DIR}/cmake/bundle_deps.sh"
              "$<TARGET_FILE:${TARGET}>"
              ""
              "${_hint_dirs_str}"
              "${PROJECT_SOURCE_DIR}/cmake/FixupBundle.cmake"
      COMMENT "bundling: copying third-party dylibs for CLI ${TARGET}"
      VERBATIM
    )
  endif()
endfunction()
