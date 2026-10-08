find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

add_library(otg_gui_support STATIC
  device_info.cpp device_info.h
  process_utils.cpp process_utils.h
  session.cpp session.h
)
target_include_directories(otg_gui_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(otg_gui_support PUBLIC Qt6::Network)

qt_add_executable(deskflow-otg-gui WIN32 MACOSX_BUNDLE
  main.cpp main_window.cpp main_window.h
)
target_link_libraries(deskflow-otg-gui PRIVATE otg_gui_support Qt6::Widgets)
set_target_properties(deskflow-otg-gui PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.gh6497.deskflow-otg"
  MACOSX_BUNDLE_BUNDLE_NAME "Deskflow OTG"
)
add_dependencies(deskflow-otg-gui deskflow-otg)
if(MSVC)
  target_compile_options(otg_gui_support PRIVATE /utf-8)
  target_compile_options(deskflow-otg-gui PRIVATE /utf-8)
  set_property(TARGET otg_gui_support deskflow-otg-gui PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(APPLE)
  add_custom_command(TARGET deskflow-otg-gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:deskflow-otg>"
      "$<TARGET_FILE_DIR:deskflow-otg-gui>/deskflow-otg")
  set(OTG_GUI_RUNTIME_DIR "deskflow-otg-gui.app/Contents/MacOS")
  install(PROGRAMS "$<TARGET_FILE:deskflow-otg>" DESTINATION "${OTG_GUI_RUNTIME_DIR}")
else()
  set(OTG_GUI_RUNTIME_DIR bin)
endif()

install(TARGETS deskflow-otg-gui BUNDLE DESTINATION . RUNTIME DESTINATION bin)
if(APPLE)
  set(gui_deploy_path "$<TARGET_FILE_NAME:deskflow-otg-gui>.app")
  set(bridge_deploy_path "${OTG_GUI_RUNTIME_DIR}/$<TARGET_FILE_NAME:deskflow-otg>")
elseif(UNIX)
  # Inspect the build binary with its Qt RPATH. An installed binary can resolve
  # a different system Qt before the bundled libraries have been copied.
  set(gui_deploy_path "$<TARGET_FILE:deskflow-otg-gui>")
  set(bridge_deploy_path "$<TARGET_FILE:deskflow-otg>")
else()
  set(gui_deploy_path "bin/$<TARGET_FILE_NAME:deskflow-otg-gui>")
  set(bridge_deploy_path "bin/$<TARGET_FILE_NAME:deskflow-otg>")
endif()
qt_generate_deploy_script(TARGET deskflow-otg-gui OUTPUT_SCRIPT deploy_script
  CONTENT "
qt_deploy_runtime_dependencies(
  EXECUTABLE \"${gui_deploy_path}\"
  ADDITIONAL_EXECUTABLES \"${bridge_deploy_path}\"
  GENERATE_QT_CONF
  NO_TRANSLATIONS
)")
install(SCRIPT ${deploy_script})

set(ADB_BUNDLE_DIR "" CACHE PATH "Extracted Android platform-tools directory to include in packages")
if(ADB_BUNDLE_DIR)
  if(NOT EXISTS "${ADB_BUNDLE_DIR}/adb${CMAKE_EXECUTABLE_SUFFIX}")
    message(FATAL_ERROR "ADB_BUNDLE_DIR must contain adb${CMAKE_EXECUTABLE_SUFFIX}")
  endif()
  install(DIRECTORY "${ADB_BUNDLE_DIR}/" DESTINATION "${OTG_GUI_RUNTIME_DIR}/platform-tools"
    USE_SOURCE_PERMISSIONS)
endif()
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/README.md"
  "${PROJECT_SOURCE_DIR}/THIRD_PARTY.md"
  DESTINATION share/deskflow-otg)

if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  find_package(Threads REQUIRED)
  qt_add_executable(test_peer ../tests/test_peer.cpp)
  target_link_libraries(test_peer PRIVATE Qt6::Network Threads::Threads)
  qt_add_executable(test_gui ../tests/test_gui.cpp)
  target_link_libraries(test_gui PRIVATE otg_gui_support Qt6::Test)
  qt_add_executable(test_window ../tests/test_window.cpp main_window.cpp main_window.h)
  target_link_libraries(test_window PRIVATE otg_gui_support Qt6::Widgets Qt6::Test)
  add_dependencies(test_gui test_peer)
  add_dependencies(test_window test_peer)
  if(MSVC)
    target_compile_options(test_gui PRIVATE /utf-8)
    target_compile_options(test_window PRIVATE /utf-8)
    set_property(TARGET test_gui test_window test_peer PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME gui_logic COMMAND test_gui)
  add_test(NAME gui_window COMMAND test_window)
  set_tests_properties(gui_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
