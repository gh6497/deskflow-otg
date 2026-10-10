find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network LinguistTools)
qt_standard_project_setup()

add_library(otg_gui_support STATIC
  device_info.cpp device_info.h
  process_utils.cpp process_utils.h
  session.cpp session.h
  i18n.cpp i18n.h
)
target_include_directories(otg_gui_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(otg_gui_support PUBLIC "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(otg_gui_support PUBLIC Qt6::Network)
# Keep all display text in the two catalogs; code references stable IDs only.
# Catalogs are maintained explicitly, not regenerated from source literals.
qt_add_lrelease(otg_gui_support
  TS_FILES translations/deskflow_otg_en.xml translations/deskflow_otg_zh_CN.xml
  OPTIONS -idbased -nounfinished
  QM_FILES_OUTPUT_VARIABLE otg_qm_files
)
qt_add_resources(otg_gui_support otg_translations
  PREFIX /i18n
  BASE "${CMAKE_CURRENT_BINARY_DIR}"
  FILES ${otg_qm_files}
)

qt_add_executable(deskflow-otg-gui WIN32 MACOSX_BUNDLE
  main.cpp main_window.cpp main_window.h
)
target_link_libraries(deskflow-otg-gui PRIVATE otg_gui_support Qt6::Widgets)
set_target_properties(deskflow-otg-gui PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.gh6497.deskflow-otg"
  MACOSX_BUNDLE_BUNDLE_NAME "Deskflow OTG"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${OTG_BUILD_VERSION}"
)
add_dependencies(deskflow-otg-gui deskflow-otg)
qt_add_resources(deskflow-otg-gui otg_app_icon
  PREFIX /icons
  BASE "${PROJECT_SOURCE_DIR}/packaging"
  FILES "${PROJECT_SOURCE_DIR}/packaging/deskflow-otg.svg"
)
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
  qt_add_resources(test_gui i18n_test_inputs
    PREFIX /i18n-test
    BASE "${PROJECT_SOURCE_DIR}"
    FILES translations/deskflow_otg_en.xml translations/deskflow_otg_zh_CN.xml
  )
  # Copy reference inputs as text: adding .cpp directly to a resource would mark
  # those shared sources HEADER_FILE_ONLY and remove them from other targets.
  set(i18n_test_sources)
  foreach(name main_window.cpp session.cpp process_utils.cpp)
    set(input "${CMAKE_CURRENT_BINARY_DIR}/i18n-test/${name}.txt")
    configure_file("${name}" "${input}" COPYONLY)
    list(APPEND i18n_test_sources "${input}")
  endforeach()
  qt_add_resources(test_gui i18n_test_sources
    PREFIX /i18n-test/gui
    BASE "${CMAKE_CURRENT_BINARY_DIR}/i18n-test"
    FILES ${i18n_test_sources}
  )
  qt_add_executable(test_window ../tests/test_window.cpp main_window.cpp main_window.h)
  qt_add_resources(test_window otg_app_icon
    PREFIX /icons
    BASE "${PROJECT_SOURCE_DIR}/packaging"
    FILES "${PROJECT_SOURCE_DIR}/packaging/deskflow-otg.svg"
  )
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
  add_test(NAME gui_version COMMAND deskflow-otg-gui --version)
  set_tests_properties(gui_version PROPERTIES
    PASS_REGULAR_EXPRESSION "deskflow-otg-gui ${PROJECT_VERSION}")
  set_tests_properties(gui_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
