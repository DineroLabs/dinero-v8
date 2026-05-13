# cmake/FixupBundle.cmake
# Called via: cmake -DAPP=... -DBUNDLE_DIR=... -DHINT_DIRS="dir1;dir2" -P FixupBundle.cmake

if(NOT APPLE)
  message(FATAL_ERROR "FixupBundle.cmake is macOS-only")
endif()

include(BundleUtilities)  # provides fixup_bundle()

if(NOT DEFINED APP)
  message(FATAL_ERROR "APP not set")
endif()

# Search directories for dependencies (Homebrew, Qt, etc.)
set(_dirs "")
if(DEFINED HINT_DIRS AND NOT "${HINT_DIRS}" STREQUAL "")
  # Convert comma-separated string back to CMake list
  string(REPLACE "," ";" _dirs "${HINT_DIRS}")
endif()

message(STATUS "fixup_bundle: APP=${APP}")
message(STATUS "fixup_bundle: DIRS=${_dirs}")

# Check if this is an app bundle or a CLI binary
get_filename_component(_app_name "${APP}" NAME_WE)
get_filename_component(_app_dir "${APP}" DIRECTORY)

if(DEFINED BUNDLE_DIR AND NOT "${BUNDLE_DIR}" STREQUAL "" AND EXISTS "${BUNDLE_DIR}")
  # This is an app bundle - use fixup_bundle normally
  message(STATUS "fixup_bundle: Processing app bundle ${BUNDLE_DIR}")
  fixup_bundle("${APP}" "" "${_dirs}")
else()
  # This is a CLI binary - use get_prerequisites and copy_and_fixup_bundle manually
  message(STATUS "fixup_bundle: Processing CLI binary ${APP}")
  
  include(GetPrerequisites)
  
  # Verify the binary exists before processing
  if(NOT EXISTS "${APP}")
    message(FATAL_ERROR "Binary does not exist: ${APP}")
  endif()
  
  # Use otool to get the actual dynamic library dependencies
  execute_process(
    COMMAND otool -L "${APP}"
    OUTPUT_VARIABLE _otool_output
    RESULT_VARIABLE _otool_result
  )
  
  if(_otool_result EQUAL 0)
    # Parse otool output to find non-system dylibs
    string(REPLACE "\n" ";" _otool_lines "${_otool_output}")
    
    foreach(_line ${_otool_lines})
      # Look for lines that contain dylib paths (skip the first line which is the binary name)
      if("${_line}" MATCHES "^\t([^(]+) \\(compatibility")
        string(REGEX REPLACE "^\t([^(]+) \\(compatibility.*" "\\1" _dylib_path "${_line}")
        string(STRIP "${_dylib_path}" _dylib_path)
        
        # Skip system libraries and already bundled libraries
        if(NOT "${_dylib_path}" MATCHES "^/usr/lib" AND 
           NOT "${_dylib_path}" MATCHES "^/System" AND
           NOT "${_dylib_path}" MATCHES "^@")
          
          get_filename_component(_dylib_name "${_dylib_path}" NAME)
          set(_dest_lib "${_app_dir}/${_dylib_name}")
          
          message(STATUS "Found non-system dylib: ${_dylib_path}")
          
          # Copy the dylib if it exists and is not already copied
          if(EXISTS "${_dylib_path}")
            if(NOT EXISTS "${_dest_lib}")
              message(STATUS "Copying ${_dylib_path} -> ${_dest_lib}")
              execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${_dylib_path}" "${_dest_lib}")
            else()
              message(STATUS "Already bundled: ${_dest_lib}")
            endif()

            # Fix install name only if current entry is an absolute path
            # Prevents install_name_tool warnings about unexpected @executable_path prefixes
            if(NOT "${_dylib_path}" MATCHES "^@executable_path")
              execute_process(
                COMMAND install_name_tool -change "${_dylib_path}" "@executable_path/${_dylib_name}" "${APP}"
                RESULT_VARIABLE _result
              )
              if(_result EQUAL 0)
                message(STATUS "Fixed install name: ${_dylib_path} -> @executable_path/${_dylib_name}")
              else()
                message(WARNING "Failed to fix install name for ${_dylib_path}")
              endif()
            else()
              message(STATUS "Skip install_name_tool (already uses @executable_path): ${_dylib_path}")
            endif()
          else()
            message(WARNING "Dylib not found: ${_dylib_path}")
          endif()
        endif()
      endif()
    endforeach()
  else()
    message(WARNING "Failed to run otool on ${APP}")
  endif()
endif()
