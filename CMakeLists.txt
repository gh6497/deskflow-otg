cmake_minimum_required(VERSION 3.16)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OTG_BASE_VERSION)
string(STRIP "${OTG_BASE_VERSION}" OTG_BASE_VERSION)
if(NOT OTG_BASE_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR "VERSION must contain a major.minor.patch version")
endif()
project(deskflow-otg VERSION ${OTG_BASE_VERSION} LANGUAGES C)
include(cmake/Version.cmake)

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
  src/usb_scan.c
  src/bridge.c
  src/deskflow_client.c
  src/keymap.c
)

option(BUILD_BLE "Build the experimental Linux BLE HID backend" ${LINUX})
if(BUILD_BLE)
  if(NOT LINUX)
    message(FATAL_ERROR "BUILD_BLE is only supported on Linux")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET gio-2.0)
  target_sources(deskflow-otg PRIVATE src/ble_hid.c src/ble_supervisor.c src/ble_link.c src/ble_session.c)
  target_link_libraries(deskflow-otg PRIVATE PkgConfig::GLIB)
  target_compile_definitions(deskflow-otg PRIVATE OTG_HAVE_BLE=1)
endif()

target_include_directories(deskflow-otg PRIVATE
  "${PROJECT_BINARY_DIR}/generated"
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
  add_executable(test_bridge_transport tests/test_bridge_transport.c src/bridge.c src/keymap.c)
  target_include_directories(test_bridge_transport PRIVATE src ${LIBUSB_INCLUDE_DIR})
  add_test(NAME bridge_transport COMMAND test_bridge_transport)
  if(BUILD_BLE)
    add_executable(test_ble_pointer tests/test_ble_pointer.c)
    target_include_directories(test_ble_pointer PRIVATE src ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(test_ble_pointer PRIVATE PkgConfig::GLIB)
    add_test(NAME ble_pointer COMMAND test_ble_pointer)
    add_executable(test_ble_supervisor tests/test_ble_supervisor.c src/ble_link.c)
    target_include_directories(test_ble_supervisor PRIVATE src)
    target_link_libraries(test_ble_supervisor PRIVATE PkgConfig::GLIB)
    add_test(NAME ble_supervisor COMMAND test_ble_supervisor)
    add_executable(test_ble_link tests/test_ble_link.c)
    target_include_directories(test_ble_link PRIVATE src)
    target_link_libraries(test_ble_link PRIVATE PkgConfig::GLIB)
    add_test(NAME ble_link COMMAND test_ble_link)
    add_executable(test_ble_session tests/test_ble_session.c)
    target_include_directories(test_ble_session PRIVATE src ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(test_ble_session PRIVATE PkgConfig::GLIB)
    add_test(NAME ble_session COMMAND test_ble_session)
    set_tests_properties(ble_session PROPERTIES TIMEOUT 30)
    add_executable(test_ble_gatt tests/test_ble_gatt.c)
    target_include_directories(test_ble_gatt PRIVATE src ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(test_ble_gatt PRIVATE PkgConfig::GLIB)
    add_test(NAME ble_gatt COMMAND test_ble_gatt)
    set_tests_properties(ble_gatt PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_test(NAME ble_latency COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ble_latency.py")
    endif()
  endif()
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
  add_test(NAME cli_version COMMAND deskflow-otg --version)
  set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "deskflow-otg ${PROJECT_VERSION}")
endif()

install(TARGETS deskflow-otg RUNTIME DESTINATION bin)
install(FILES "${PROJECT_BINARY_DIR}/BUILD_VERSION.txt" DESTINATION share/deskflow-otg)

option(BUILD_GUI "Build the Qt desktop application" OFF)
if(BUILD_GUI)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  add_subdirectory(gui)
endif()
