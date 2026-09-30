cmake_minimum_required(VERSION 3.16)
project(deskflow-otg C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Locate libusb-1.0 without requiring pkg-config.
find_path(LIBUSB_INCLUDE_DIR
  NAMES libusb-1.0/libusb.h
  HINTS /usr/include /usr/local/include
)

find_library(LIBUSB_LIBRARY
  NAMES usb-1.0
  HINTS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)

if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
  message(FATAL_ERROR
    "libusb-1.0 not found.\n"
    "Install the development package, e.g.:\n"
    "    sudo apt install libusb-1.0-0-dev")
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

install(TARGETS deskflow-otg RUNTIME DESTINATION bin)
