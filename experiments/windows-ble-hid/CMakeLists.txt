cmake_minimum_required(VERSION 3.16)
project(deskflow_otg_windows_ble_probe LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This experiment needs Windows, MSVC and a Windows 10/11 SDK with C++/WinRT headers")
endif()

add_executable(deskflow-otg-ble-probe main.cpp)
# Standard coroutines for asynchronous WinRT GATT request handlers. This does
# not change the language requirements of the project's C11 bridge or Qt GUI.
target_compile_features(deskflow-otg-ble-probe PRIVATE cxx_std_20)
target_compile_definitions(deskflow-otg-ble-probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(deskflow-otg-ble-probe PRIVATE /W4 /WX /EHsc /utf-8)
target_link_libraries(deskflow-otg-ble-probe PRIVATE windowsapp)
# Standalone executable: no Qt, libusb, vcpkg or separately installed VC runtime.
set_property(TARGET deskflow-otg-ble-probe PROPERTY MSVC_RUNTIME_LIBRARY
  "MultiThreaded$<$<CONFIG:Debug>:Debug>")
enable_testing()
add_test(NAME ble_probe_packets COMMAND deskflow-otg-ble-probe --self-test)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test_ble_win_queue.cpp")
  add_executable(test_ble_win_queue ../../tests/test_ble_win_queue.cpp)
  target_include_directories(test_ble_win_queue PRIVATE ../../platform/windows)
  target_compile_features(test_ble_win_queue PRIVATE cxx_std_17)
  target_compile_options(test_ble_win_queue PRIVATE /W4 /WX /EHsc)
  add_test(NAME ble_win_queue COMMAND test_ble_win_queue)
  add_executable(test_ble_win_wait ../../tests/test_ble_win_wait.cpp)
  target_include_directories(test_ble_win_wait PRIVATE ../../platform/windows)
  target_compile_features(test_ble_win_wait PRIVATE cxx_std_17)
  target_compile_options(test_ble_win_wait PRIVATE /W4 /WX /EHsc)
  add_test(NAME ble_win_wait COMMAND test_ble_win_wait)
endif()
