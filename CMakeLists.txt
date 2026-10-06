cmake_minimum_required(VERSION 3.16)
project(deskflow-otg C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Locate libusb-1.0 without requiring pkg-config.
set(LIBUSB_ROOT_HINTS)
if(APPLE)
  # Homebrew: Apple Silicon and Intel, including the libusb opt symlinks.
  list(APPEND LIBUSB_ROOT_HINTS
    /opt/homebrew /opt/homebrew/opt/libusb
    /usr/local /usr/local/opt/libusb
  )
endif()

find_path(LIBUSB_INCLUDE_DIR
  NAMES libusb-1.0/libusb.h
  HINTS ${LIBUSB_ROOT_HINTS}
  PATH_SUFFIXES include
)

find_library(LIBUSB_LIBRARY
  NAMES usb-1.0 libusb-1.0
  HINTS ${LIBUSB_ROOT_HINTS}
  PATH_SUFFIXES lib
)

# vcpkg exposes both debug/lib and lib on multi-config Windows generators.
# Do not accidentally link the Debug library into a Release executable.
if(WIN32 AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
  find_library(LIBUSB_RELEASE_LIBRARY
    NAMES libusb-1.0 usb-1.0
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
    NO_DEFAULT_PATH
  )
  find_library(LIBUSB_DEBUG_LIBRARY
    NAMES libusb-1.0 usb-1.0
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
    NO_DEFAULT_PATH
  )
  if(LIBUSB_RELEASE_LIBRARY)
    set(LIBUSB_LIBRARY "${LIBUSB_RELEASE_LIBRARY}")
  endif()
endif()

if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
  message(FATAL_ERROR
    "libusb-1.0 not found.\n"
    "Install the development package, e.g.:\n"
    "    Linux: sudo apt install libusb-1.0-0-dev\n"
    "    macOS: brew install libusb\n"
    "    Windows: vcpkg install libusb:x64-windows-static\n"
    "For a custom prefix, set -DCMAKE_PREFIX_PATH=<libusb-prefix>.")
endif()

add_executable(deskflow-otg
  src/main.c
  src/aoa_hid.c
  src/bridge.c
  src/deskflow_client.c
  src/keymap.c
)

target_include_directories(deskflow-otg PRIVATE
  src
  ${LIBUSB_INCLUDE_DIR}
)

if(LIBUSB_RELEASE_LIBRARY AND LIBUSB_DEBUG_LIBRARY)
  target_link_libraries(deskflow-otg PRIVATE
    optimized "${LIBUSB_RELEASE_LIBRARY}" debug "${LIBUSB_DEBUG_LIBRARY}")
else()
  target_link_libraries(deskflow-otg PRIVATE ${LIBUSB_LIBRARY})
endif()
if(WIN32)
  # The vcpkg static libusb package does not supply transitive link metadata.
  target_link_libraries(deskflow-otg PRIVATE ws2_32 setupapi cfgmgr32 advapi32 ole32)
  if(VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set_property(TARGET deskflow-otg PROPERTY MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(test_socket tests/test_socket.c)
  target_include_directories(test_socket PRIVATE src)
  if(WIN32)
    target_link_libraries(test_socket PRIVATE ws2_32)
    if(VCPKG_TARGET_TRIPLET MATCHES "-static$")
      set_property(TARGET test_socket PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
  endif()
  add_test(NAME socket_io COMMAND test_socket)
endif()

install(TARGETS deskflow-otg RUNTIME DESTINATION bin)
