# Packaging targets for all platforms

# macOS packaging
if(APPLE)
    add_custom_target(pkg_mac
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/mac/build-installer.sh
                --version ${CMAKE_PROJECT_VERSION}
                --build-dir ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Creating macOS release package"
    )
endif()

# Windows packaging
if(WIN32)
    add_custom_target(pkg_win
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/windows/build-installer.ps1
                -Version ${CMAKE_PROJECT_VERSION}
                -DaemonBuildDir ${CMAKE_BINARY_DIR}/$<CONFIG>
                -QtBuildDir ${CMAKE_BINARY_DIR}/qt/$<CONFIG>
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Creating Windows release installer"
    )
endif()

# Linux packaging
if(UNIX AND NOT APPLE)
    add_custom_target(pkg_linux
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/linux/build-installer.sh
                --version ${CMAKE_PROJECT_VERSION}
                --build-dir ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Creating Linux release package"
    )
endif()

# Universal packaging target
add_custom_target(pkg_all
    COMMENT "Creating packages for all supported platforms"
)

if(APPLE)
    add_dependencies(pkg_all pkg_mac)
endif()

if(WIN32)
    add_dependencies(pkg_all pkg_win)
endif()

if(UNIX AND NOT APPLE)
    add_dependencies(pkg_all pkg_linux)
endif()
