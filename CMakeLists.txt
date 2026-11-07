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

if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH
    /opt/homebrew/opt/openssl@3 /opt/homebrew/opt/openssl
    /usr/local/opt/openssl@3 /usr/local/opt/openssl)
endif()
if(WIN32 AND NOT VCPKG_INSTALLED_DIR AND DEFINED ENV{VCPKG_INSTALLATION_ROOT})
  # Allow a developer shell to use the installed vcpkg packages even when
  # CMake was invoked without vcpkg.cmake. The toolchain remains preferred.
  set(VCPKG_INSTALLED_DIR "$ENV{VCPKG_INSTALLATION_ROOT}/installed"
      CACHE PATH "vcpkg installed package root")
endif()
if(WIN32 AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
  list(APPEND CMAKE_PREFIX_PATH
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
if(WIN32 AND DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT OPENSSL_ROOT_DIR)
  set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}" CACHE PATH
      "OpenSSL installation root")
endif()
find_package(OpenSSL QUIET COMPONENTS SSL Crypto)
if(NOT OpenSSL_FOUND)
  if(WIN32)
    message(FATAL_ERROR
      "OpenSSL was not found. Install it with vcpkg and configure with:\n"
      "  vcpkg install libusb:x64-windows-static openssl:x64-windows-static\n"
      "  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static\n"
      "For BUILD_GUI=ON, also install openssl:x64-windows-static-md.\n"
      "Alternatively set OPENSSL_ROOT_DIR to an OpenSSL installation.")
  endif()
  message(FATAL_ERROR "OpenSSL with SSL and Crypto components is required")
endif()

set(OTG_PROFILE "manual" CACHE STRING
  "Feature profile: manual keeps individual options, full enables all platform-supported features")
set_property(CACHE OTG_PROFILE PROPERTY STRINGS manual full)

option(BUILD_AUDIO "Build scrcpy-compatible ADB audio forwarding" OFF)
option(BUILD_CLIPBOARD "Build Deskflow text clipboard sharing via ADB/scrcpy" OFF)
option(BUILD_GUI "Build the Qt desktop application" OFF)
option(BUILD_BLE "Build the Linux BLE HID backend" ${LINUX})
option(BUILD_BLE_WIN "Build the experimental Windows BLE HID backend (CLI/GUI, MSVC/Windows SDK)" OFF)

if(NOT OTG_PROFILE STREQUAL "manual" AND NOT OTG_PROFILE STREQUAL "full")
  message(FATAL_ERROR "OTG_PROFILE must be manual or full, got: ${OTG_PROFILE}")
endif()
if(OTG_PROFILE STREQUAL "full")
  # "full" means all features supported by the current host platform. It does
  # not try to build another platform's backend (for example Windows BLE on
  # Linux), and it keeps macOS audio disabled because that backend is absent.
  set(BUILD_GUI ON CACHE BOOL "Build the Qt desktop application" FORCE)
  set(BUILD_CLIPBOARD ON CACHE BOOL "Build Deskflow text clipboard sharing via ADB/scrcpy" FORCE)
  if(LINUX)
    set(BUILD_AUDIO ON CACHE BOOL "Build scrcpy-compatible ADB audio forwarding" FORCE)
    set(BUILD_BLE ON CACHE BOOL "Build the Linux BLE HID backend" FORCE)
    set(BUILD_BLE_WIN OFF CACHE BOOL "Build the experimental Windows BLE HID backend (CLI/GUI, MSVC/Windows SDK)" FORCE)
  elseif(WIN32)
    set(BUILD_AUDIO ON CACHE BOOL "Build scrcpy-compatible ADB audio forwarding" FORCE)
    set(BUILD_BLE OFF CACHE BOOL "Build the Linux BLE HID backend" FORCE)
    set(BUILD_BLE_WIN ON CACHE BOOL "Build the experimental Windows BLE HID backend (CLI/GUI, MSVC/Windows SDK)" FORCE)
  elseif(APPLE)
    set(BUILD_AUDIO OFF CACHE BOOL "Build scrcpy-compatible ADB audio forwarding" FORCE)
    set(BUILD_BLE OFF CACHE BOOL "Build the Linux BLE HID backend" FORCE)
    set(BUILD_BLE_WIN OFF CACHE BOOL "Build the experimental Windows BLE HID backend (CLI/GUI, MSVC/Windows SDK)" FORCE)
  else()
    message(FATAL_ERROR "OTG_PROFILE=full is unsupported on this platform")
  endif()
endif()
if(BUILD_CLIPBOARD AND NOT (LINUX OR APPLE OR WIN32))
  message(FATAL_ERROR "BUILD_CLIPBOARD currently supports Linux, macOS and Windows")
endif()
set(OTG_BUNDLED_SCRCPY_VERSION "5.0.1")
set(OTG_BUNDLED_SCRCPY_SERVER
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/scrcpy/scrcpy-server-v${OTG_BUNDLED_SCRCPY_VERSION}")
set(SCRCPY_SERVER "${OTG_BUNDLED_SCRCPY_SERVER}" CACHE FILEPATH
  "Matching scrcpy-server jar/apk to bundle for audio/clipboard")
set(SCRCPY_VERSION "${OTG_BUNDLED_SCRCPY_VERSION}" CACHE STRING
  "Version of the bundled SCRCPY_SERVER")

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
  src/clipboard.c
  src/adb_target.c
  src/adb_display.c
)
if((LINUX OR APPLE) AND (BUILD_AUDIO OR BUILD_CLIPBOARD))
  target_sources(deskflow-otg PRIVATE platform/posix/scrcpy_session.c)
elseif(WIN32 AND (BUILD_AUDIO OR BUILD_CLIPBOARD))
  target_sources(deskflow-otg PRIVATE platform/windows/scrcpy_session.c)
else()
  target_sources(deskflow-otg PRIVATE src/scrcpy_session_stub.c)
endif()

if(BUILD_AUDIO OR BUILD_CLIPBOARD)
  if(NOT SCRCPY_SERVER)
    message(FATAL_ERROR
      "ADB features require SCRCPY_SERVER; provide the matching scrcpy-server "
      "file to bundle with the application")
  endif()
  if(NOT SCRCPY_VERSION)
    message(FATAL_ERROR
      "ADB features require SCRCPY_VERSION for the bundled scrcpy-server")
  endif()
  if(BUILD_CLIPBOARD)
    target_compile_definitions(deskflow-otg PRIVATE OTG_HAVE_CLIPBOARD=1)
  endif()
  if(BUILD_AUDIO)
    target_compile_definitions(deskflow-otg PRIVATE OTG_HAVE_AUDIO=1)
    if(LINUX)
      target_sources(deskflow-otg PRIVATE platform/linux/audio_player.c)
      find_package(ALSA REQUIRED)
      find_package(Threads REQUIRED)
      target_link_libraries(deskflow-otg PRIVATE ALSA::ALSA Threads::Threads)
    elseif(WIN32)
      target_sources(deskflow-otg PRIVATE platform/windows/audio_player.c)
      target_link_libraries(deskflow-otg PRIVATE ole32 uuid)
    else()
      message(FATAL_ERROR "BUILD_AUDIO currently supports Linux and Windows")
    endif()
  endif()
  if(NOT EXISTS "${SCRCPY_SERVER}")
    message(FATAL_ERROR "SCRCPY_SERVER does not exist: ${SCRCPY_SERVER}")
  endif()
  install(FILES "${SCRCPY_SERVER}" DESTINATION share/deskflow-otg
          RENAME scrcpy-server)
  file(WRITE "${PROJECT_BINARY_DIR}/SCRCPY_VERSION.txt" "${SCRCPY_VERSION}\n")
  install(FILES "${PROJECT_BINARY_DIR}/SCRCPY_VERSION.txt"
          DESTINATION share/deskflow-otg)
endif()

if(BUILD_BLE_WIN)
  if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "BUILD_BLE_WIN requires Windows, MSVC and a Windows SDK with C++/WinRT")
  endif()
  enable_language(CXX)
  target_sources(deskflow-otg PRIVATE platform/windows/ble_win.cpp platform/windows/ble_win_control.c)
  target_include_directories(deskflow-otg PRIVATE platform/windows)
  # Coroutine support is confined to this adapter; core C and GUI stay C11/C++17.
  target_compile_features(deskflow-otg PRIVATE cxx_std_20)
  target_compile_definitions(deskflow-otg PRIVATE OTG_HAVE_BLE_WIN=1 NOMINMAX WIN32_LEAN_AND_MEAN)
  # VS generators cannot separate target-wide C/C++ warning options reliably.
  set_source_files_properties(platform/windows/ble_win.cpp PROPERTIES COMPILE_OPTIONS "/W4;/WX;/EHsc;/utf-8")
  target_link_libraries(deskflow-otg PRIVATE windowsapp)
endif()
if(BUILD_BLE)
  if(NOT LINUX)
    message(FATAL_ERROR "BUILD_BLE is only supported on Linux")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET gio-2.0)
  target_sources(deskflow-otg PRIVATE
    platform/linux/ble_hid.c
    platform/linux/ble_supervisor.c
    platform/linux/ble_link.c
    platform/linux/ble_session.c
  )
  target_include_directories(deskflow-otg PRIVATE platform/linux)
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
target_link_libraries(deskflow-otg PRIVATE OpenSSL::SSL OpenSSL::Crypto)
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
  add_executable(test_ble_win_control tests/test_ble_win_control.c platform/windows/ble_win_control.c)
  target_include_directories(test_ble_win_control PRIVATE platform/windows)
  add_test(NAME ble_win_control COMMAND test_ble_win_control)
  if(BUILD_BLE_WIN)
    add_executable(test_ble_win_control_peer src/main.c platform/windows/ble_win_control.c src/bridge.c
      src/keymap.c src/aoa_hid.c src/usb_scan.c src/scrcpy_session_stub.c src/clipboard.c src/adb_display.c tests/ble_win_control_peer.c)
    target_compile_definitions(test_ble_win_control_peer PRIVATE OTG_HAVE_BLE_WIN=1)
    target_include_directories(test_ble_win_control_peer PRIVATE src platform/windows
      "${PROJECT_BINARY_DIR}/generated" ${LIBUSB_INCLUDE_DIR})
    get_target_property(OTG_NATIVE_LINK_LIBS deskflow-otg LINK_LIBRARIES)
    target_link_libraries(test_ble_win_control_peer PRIVATE ${OTG_NATIVE_LINK_LIBS})
    get_target_property(OTG_NATIVE_RUNTIME deskflow-otg MSVC_RUNTIME_LIBRARY)
    if(OTG_NATIVE_RUNTIME)
      set_property(TARGET test_ble_win_control_peer PROPERTY MSVC_RUNTIME_LIBRARY "${OTG_NATIVE_RUNTIME}")
    endif()
    add_executable(test_ble_win_queue tests/test_ble_win_queue.cpp)
    target_include_directories(test_ble_win_queue PRIVATE platform/windows)
    target_compile_features(test_ble_win_queue PRIVATE cxx_std_17)
    add_test(NAME ble_win_queue COMMAND test_ble_win_queue)
    add_executable(test_ble_win_bridge tests/test_ble_win_bridge.cpp src/bridge.c src/keymap.c)
    target_include_directories(test_ble_win_bridge PRIVATE src platform/windows ${LIBUSB_INCLUDE_DIR})
    target_compile_features(test_ble_win_bridge PRIVATE cxx_std_17)
    add_test(NAME ble_win_bridge COMMAND test_ble_win_bridge)
    add_executable(test_ble_win_wait tests/test_ble_win_wait.cpp)
    target_include_directories(test_ble_win_wait PRIVATE platform/windows)
    target_compile_features(test_ble_win_wait PRIVATE cxx_std_17)
    add_test(NAME ble_win_wait COMMAND test_ble_win_wait)
  endif()
  add_executable(test_bridge_transport tests/test_bridge_transport.c src/bridge.c src/keymap.c)
  target_include_directories(test_bridge_transport PRIVATE src ${LIBUSB_INCLUDE_DIR})
  add_test(NAME bridge_transport COMMAND test_bridge_transport)
  if(BUILD_BLE)
    add_executable(test_ble_pointer tests/test_ble_pointer.c)
    target_include_directories(test_ble_pointer PRIVATE src ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(test_ble_pointer PRIVATE PkgConfig::GLIB)
    add_test(NAME ble_pointer COMMAND test_ble_pointer)
    add_executable(test_ble_supervisor tests/test_ble_supervisor.c platform/linux/ble_link.c)
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
  add_executable(test_socket tests/test_socket.c src/clipboard.c)
  target_include_directories(test_socket PRIVATE src)
  target_link_libraries(test_socket PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  if(WIN32)
    target_link_libraries(test_socket PRIVATE ws2_32)
    if(VCPKG_TARGET_TRIPLET MATCHES "-static$")
      set_property(TARGET test_socket PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
  endif()
  add_test(NAME socket_io COMMAND test_socket)
  add_executable(test_tls tests/test_tls.c src/clipboard.c)
  target_include_directories(test_tls PRIVATE src)
  target_link_libraries(test_tls PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  add_test(NAME tls_fingerprint COMMAND test_tls)
  add_executable(test_clipboard tests/test_clipboard.c src/clipboard.c)
  target_include_directories(test_clipboard PRIVATE src)
  target_link_libraries(test_clipboard PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  if(WIN32)
    target_link_libraries(test_clipboard PRIVATE ws2_32)
  endif()
  add_test(NAME clipboard_protocol COMMAND test_clipboard)
  add_executable(test_adb_target tests/test_adb_target.c src/adb_target.c)
  target_include_directories(test_adb_target PRIVATE src)
  add_test(NAME adb_target COMMAND test_adb_target)
  add_executable(test_adb_display tests/test_adb_display.c)
  target_include_directories(test_adb_display PRIVATE src)
  add_test(NAME adb_display COMMAND test_adb_display)
  set_tests_properties(adb_display PROPERTIES TIMEOUT 20)
  set_tests_properties(clipboard_protocol PROPERTIES TIMEOUT 15)
  if(UNIX)
    add_executable(test_clipboard_bridge tests/test_clipboard_bridge.c src/clipboard.c
      src/bridge.c src/keymap.c src/scrcpy_session_stub.c src/usb_scan.c src/adb_display.c)
    target_include_directories(test_clipboard_bridge PRIVATE src "${PROJECT_BINARY_DIR}/generated" ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(test_clipboard_bridge PRIVATE ${LIBUSB_LIBRARY} OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME clipboard_bridge COMMAND test_clipboard_bridge)
    set_tests_properties(clipboard_bridge PROPERTIES TIMEOUT 15)
  endif()
  if(BUILD_CLIPBOARD AND UNIX)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_executable(test_auxiliary tests/test_auxiliary.c src/clipboard.c src/adb_target.c platform/posix/scrcpy_session.c)
      target_include_directories(test_auxiliary PRIVATE src)
      target_compile_definitions(test_auxiliary PRIVATE OTG_HAVE_CLIPBOARD=1)
      if(BUILD_AUDIO)
        target_sources(test_auxiliary PRIVATE platform/linux/audio_player.c)
        target_compile_definitions(test_auxiliary PRIVATE OTG_HAVE_AUDIO=1)
        target_link_libraries(test_auxiliary PRIVATE ALSA::ALSA Threads::Threads)
      endif()
      add_test(NAME clipboard_auxiliary COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/test_auxiliary.py" "$<TARGET_FILE:test_auxiliary>" "${BUILD_AUDIO}")
      set_tests_properties(clipboard_auxiliary PROPERTIES TIMEOUT 60)
    endif()
  endif()
  add_test(NAME cli_version COMMAND deskflow-otg --version)
  if(BUILD_AUDIO AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_audio_ready tests/test_audio_ready.c platform/linux/audio_player.c)
    target_include_directories(test_audio_ready PRIVATE src)
    target_link_libraries(test_audio_ready PRIVATE ALSA::ALSA Threads::Threads)
    # The assertions perform socket I/O, including in release builds.
    target_compile_options(test_audio_ready PRIVATE -UNDEBUG)
    add_test(NAME audio_ready COMMAND test_audio_ready)
    set_tests_properties(audio_ready PROPERTIES TIMEOUT 10
      ENVIRONMENT "ALSA_CONFIG_PATH=${PROJECT_SOURCE_DIR}/tests/alsa-null.conf")
  endif()
  # Optional host-only Android policy regression, never an Android SDK/JDK
  # requirement for the C11 bridge or the Linux/Windows/macOS GUI.
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME companion_signing COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/test_sign_companion_apk.py")
    add_test(NAME android_layouts COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/test_android_layouts.py")
    set_tests_properties(android_layouts PROPERTIES TIMEOUT 10)
    add_test(NAME android_usb_disconnect COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/test_android_usb_disconnect.py")
    set_tests_properties(android_usb_disconnect PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
    add_test(NAME android_connection_receiver COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/test_android_connection_receiver.py")
    set_tests_properties(android_connection_receiver PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
    add_test(NAME android_transfer COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/test_android_transfer.py")
    set_tests_properties(android_transfer PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
  endif()
  set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "deskflow-otg ${PROJECT_VERSION}")
  add_test(NAME cli_ble_validation COMMAND "${CMAKE_COMMAND}"
    "-DCLI=$<TARGET_FILE:deskflow-otg>" "-DWINDOWS_BLE=${BUILD_BLE_WIN}" "-DCLIPBOARD=${BUILD_CLIPBOARD}"
    -P "${PROJECT_SOURCE_DIR}/tests/test_cli_ble.cmake")
endif()

install(TARGETS deskflow-otg RUNTIME DESTINATION bin)
install(FILES "${PROJECT_BINARY_DIR}/BUILD_VERSION.txt" DESTINATION share/deskflow-otg)
if(BUILD_BLE)
  set(OTG_BLE_FEATURE true)
else()
  set(OTG_BLE_FEATURE false)
endif()
if(BUILD_BLE_WIN)
  set(OTG_BLE_WIN_FEATURE true)
else()
  set(OTG_BLE_WIN_FEATURE false)
endif()
# 'ble' remains the Linux resident feature. Keep windowsBleCli for compatibility;
# the same Windows backend also supports GUI-owned foreground sessions.
if(BUILD_AUDIO)
  set(OTG_AUDIO_FEATURE true)
else()
  set(OTG_AUDIO_FEATURE false)
endif()
if(BUILD_CLIPBOARD)
  set(OTG_CLIPBOARD_FEATURE true)
else()
  set(OTG_CLIPBOARD_FEATURE false)
endif()
if(BUILD_GUI)
  set(OTG_GUI_FEATURE true)
else()
  set(OTG_GUI_FEATURE false)
endif()
file(WRITE "${PROJECT_BINARY_DIR}/BUILD_FEATURES.json"
  "{\"profile\":\"${OTG_PROFILE}\",\"gui\":${OTG_GUI_FEATURE},\"audio\":${OTG_AUDIO_FEATURE},\"clipboard\":${OTG_CLIPBOARD_FEATURE},\"ble\":${OTG_BLE_FEATURE},\"windowsBleCli\":${OTG_BLE_WIN_FEATURE}}\n")
message(STATUS "Deskflow OTG feature profile: ${OTG_PROFILE}")
message(STATUS "  GUI: ${BUILD_GUI}")
message(STATUS "  Audio: ${BUILD_AUDIO}")
message(STATUS "  Clipboard: ${BUILD_CLIPBOARD}")
message(STATUS "  Linux BLE: ${BUILD_BLE}")
message(STATUS "  Windows BLE: ${BUILD_BLE_WIN}")
install(FILES "${PROJECT_BINARY_DIR}/BUILD_FEATURES.json" DESTINATION share/deskflow-otg)
if(LINUX)
  # pkexec strips LD_LIBRARY_PATH. Both native CLI and elevated supervisor must
  # resolve the staged runtime without relying on the AppRun environment.
  set_target_properties(deskflow-otg PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(BUILD_GUI)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  add_subdirectory(gui)
  if(BUILD_AUDIO)
    target_compile_definitions(otg_gui_support PUBLIC OTG_GUI_HAVE_AUDIO=1)
  endif()
  if(BUILD_CLIPBOARD)
    target_compile_definitions(otg_gui_support PUBLIC OTG_GUI_HAVE_CLIPBOARD=1)
  endif()
endif()
