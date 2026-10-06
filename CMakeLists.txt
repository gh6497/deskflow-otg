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
  NAMES usb-1.0
  HINTS ${LIBUSB_ROOT_HINTS}
  PATH_SUFFIXES lib
)

if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
  message(FATAL_ERROR
    "libusb-1.0 not found.\n"
    "Install the development package, e.g.:\n"
    "    Linux: sudo apt install libusb-1.0-0-dev\n"
    "    macOS: brew install libusb\n"
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

target_link_libraries(deskflow-otg PRIVATE
  ${LIBUSB_LIBRARY}
)

include(CTest)
if(BUILD_TESTING)
  add_executable(test_socket tests/test_socket.c)
  target_include_directories(test_socket PRIVATE src)
  add_test(NAME socket_io COMMAND test_socket)
endif()

install(TARGETS deskflow-otg RUNTIME DESTINATION bin)
