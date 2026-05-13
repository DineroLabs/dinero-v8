# Custom CMake build for libusb 1.0.27
# This is a minimal CMake wrapper for vendoring libusb in DineroCoin
cmake_minimum_required(VERSION 3.15)
project(libusb-1.0 VERSION 1.0.27 LANGUAGES C)

# Platform-specific source files
if(APPLE)
    set(PLATFORM_SOURCES
        libusb/os/darwin_usb.c
        libusb/os/events_posix.c
        libusb/os/threads_posix.c
    )
    set(PLATFORM_LIBS "-framework IOKit" "-framework CoreFoundation" "-framework Security")

elseif(WIN32)
    set(PLATFORM_SOURCES
        libusb/os/windows_common.c
        libusb/os/windows_usbdk.c
        libusb/os/windows_winusb.c
        libusb/os/events_windows.c
        libusb/os/threads_windows.c
    )
    set(PLATFORM_LIBS)

elseif(UNIX)
    # Linux
    set(PLATFORM_SOURCES
        libusb/os/linux_usbfs.c
        libusb/os/events_posix.c
        libusb/os/threads_posix.c
    )
    # Check for libudev
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(UDEV libudev)
        if(UDEV_FOUND)
            list(APPEND PLATFORM_SOURCES libusb/os/linux_udev.c)
            set(PLATFORM_LIBS ${UDEV_LIBRARIES})
            add_definitions(-DHAVE_LIBUDEV)
        else()
            list(APPEND PLATFORM_SOURCES libusb/os/linux_netlink.c)
        endif()
    else()
        list(APPEND PLATFORM_SOURCES libusb/os/linux_netlink.c)
    endif()
endif()

# Core libusb sources
set(LIBUSB_SOURCES
    libusb/core.c
    libusb/descriptor.c
    libusb/hotplug.c
    libusb/io.c
    libusb/strerror.c
    libusb/sync.c
    ${PLATFORM_SOURCES}
)

# Create static library
add_library(usb-1.0 STATIC ${LIBUSB_SOURCES})

# Include directories
target_include_directories(usb-1.0 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb
    ${CMAKE_CURRENT_SOURCE_DIR}  # For config.h
)

# Platform-specific definitions
if(APPLE)
    target_compile_definitions(usb-1.0 PRIVATE
        OS_DARWIN=1
        _DARWIN_C_SOURCE
    )
elseif(WIN32)
    target_compile_definitions(usb-1.0 PRIVATE
        OS_WINDOWS=1
        _WIN32_WINNT=0x0600
    )
elseif(UNIX)
    target_compile_definitions(usb-1.0 PRIVATE
        OS_LINUX=1
        _GNU_SOURCE
    )
endif()

# Link platform libraries
if(PLATFORM_LIBS)
    target_link_libraries(usb-1.0 PUBLIC ${PLATFORM_LIBS})
endif()

# Set output name to match system libusb
set_target_properties(usb-1.0 PROPERTIES
    OUTPUT_NAME "usb-1.0"
    VERSION ${PROJECT_VERSION}
)

# Installation
install(TARGETS usb-1.0
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES libusb/libusb.h
    DESTINATION include/libusb-1.0
)
