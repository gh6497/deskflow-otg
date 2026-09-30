cmake_minimum_required(VERSION 3.16)
project(deskflow-otg C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_executable(deskflow-otg
  src/main.c
  src/aoa_hid.c
  src/bridge.c
  src/deskflow_client.c
  src/keymap.c
)

target_include_directories(deskflow-otg PRIVATE src)

target_link_libraries(deskflow-otg PRIVATE
  PkgConfig::LIBUSB
)

install(TARGETS deskflow-otg RUNTIME DESTINATION bin)
