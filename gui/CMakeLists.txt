find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network LinguistTools)
qt_standard_project_setup()

add_library(otg_gui_support STATIC
  device_info.cpp device_info.h
  process_utils.cpp process_utils.h
  session.cpp session.h
  deskflow_server.cpp deskflow_server.h
  connection_manager.cpp connection_manager.h
  companion_app.cpp companion_app.h
  companion_status_sync.cpp companion_status_sync.h
  companion_transfer.cpp companion_transfer.h
  transfer_dialog.cpp transfer_dialog.h
  usb_auto_connect.cpp usb_auto_connect.h
  tls_certificate.cpp tls_certificate.h
  i18n.cpp i18n.h
  system_theme.cpp system_theme.h
)
target_include_directories(otg_gui_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(otg_gui_support PUBLIC "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(otg_gui_support PUBLIC Qt6::Network Qt6::Widgets)
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-windows-static$")
  # The bridge uses /MT with static libusb, while Qt uses /MD. Link the GUI
  # against a static OpenSSL built with /MD rather than mixing both CRTs.
  set(gui_openssl_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}-md")
  find_library(OTG_GUI_CRYPTO_RELEASE NAMES libcrypto PATHS "${gui_openssl_root}/lib" NO_DEFAULT_PATH)
  find_library(OTG_GUI_CRYPTO_DEBUG NAMES libcrypto PATHS "${gui_openssl_root}/debug/lib" NO_DEFAULT_PATH)
  if(NOT OTG_GUI_CRYPTO_RELEASE OR NOT OTG_GUI_CRYPTO_DEBUG)
    message(FATAL_ERROR "GUI certificate generation requires: vcpkg install openssl:${VCPKG_TARGET_TRIPLET}-md")
  endif()
  target_include_directories(otg_gui_support PUBLIC "${gui_openssl_root}/include")
  target_link_libraries(otg_gui_support PUBLIC
    optimized "${OTG_GUI_CRYPTO_RELEASE}" debug "${OTG_GUI_CRYPTO_DEBUG}" crypt32 ws2_32)
else()
  target_link_libraries(otg_gui_support PUBLIC OpenSSL::Crypto)
endif()
if(BUILD_BLE)
  target_compile_definitions(otg_gui_support PUBLIC OTG_GUI_HAVE_BLE=1)
endif()
if(BUILD_BLE_WIN)
  target_compile_definitions(otg_gui_support PUBLIC OTG_GUI_HAVE_BLE_WIN=1)
endif()
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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Platform themes are not imported by default. Deploy plugins from this Qt
  # installation (never from an unrelated system Qt), but let Qt/the desktop
  # select the theme at runtime. Missing optional plugins retain Qt's fallback.
  foreach(theme_plugin QGtk3ThemePlugin QXdgDesktopPortalThemePlugin)
    if(TARGET Qt6::${theme_plugin})
      qt_import_plugins(deskflow-otg-gui INCLUDE Qt6::${theme_plugin})
    endif()
  endforeach()
endif()
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
# A build-tree GUI must find the same matching runtime pair as an installed
# GUI. Keep the copy target incremental-build safe when only the server changes.
if(APPLE)
  set(OTG_GUI_BUILD_DATA_DIR "$<TARGET_FILE_DIR:deskflow-otg-gui>/../Resources/deskflow-otg")
else()
  set(OTG_GUI_BUILD_DATA_DIR "$<TARGET_FILE_DIR:deskflow-otg-gui>/share/deskflow-otg")
endif()
if(BUILD_AUDIO OR BUILD_CLIPBOARD)
  add_custom_target(otg_gui_scrcpy_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OTG_GUI_BUILD_DATA_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCRCPY_SERVER}"
      "${OTG_GUI_BUILD_DATA_DIR}/scrcpy-server"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_BINARY_DIR}/SCRCPY_VERSION.txt"
      "${OTG_GUI_BUILD_DATA_DIR}/SCRCPY_VERSION.txt"
    DEPENDS deskflow-otg-gui "${SCRCPY_SERVER}" "${PROJECT_BINARY_DIR}/SCRCPY_VERSION.txt"
    VERBATIM)
endif()
# Supply an explicitly signed APK; desktop builds never require an Android SDK
# or silently ship an unsigned release/debug APK from a developer's build tree.
set(COMPANION_APK "" CACHE FILEPATH "Signed Android companion APK to bundle with the GUI")
if(COMPANION_APK)
  get_filename_component(companion_apk "${COMPANION_APK}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
  if(NOT EXISTS "${companion_apk}" OR IS_DIRECTORY "${companion_apk}" OR NOT companion_apk MATCHES "\\.[aA][pP][kK]$")
    message(FATAL_ERROR "COMPANION_APK must point to a signed .apk file")
  endif()
  add_custom_target(otg_companion_apk ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${companion_apk}"
      "$<TARGET_FILE_DIR:deskflow-otg-gui>/deskflow-otg-companion.apk"
    DEPENDS deskflow-otg-gui "${companion_apk}"
    VERBATIM)
  install(FILES "${companion_apk}" DESTINATION "${OTG_GUI_RUNTIME_DIR}" RENAME deskflow-otg-companion.apk)
endif()
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
  add_custom_target(otg_gui_adb_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ADB_BUNDLE_DIR}"
      "$<TARGET_FILE_DIR:deskflow-otg-gui>/platform-tools"
    DEPENDS deskflow-otg-gui
    VERBATIM)
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
  qt_add_executable(test_companion_status ../tests/test_companion_status.cpp)
  qt_add_executable(test_transfer_peer ../tests/test_transfer_peer.cpp)
  target_link_libraries(test_transfer_peer PRIVATE Qt6::Core)
  qt_add_executable(test_companion_transfer ../tests/test_companion_transfer.cpp)
  target_link_libraries(test_companion_transfer PRIVATE otg_gui_support Qt6::Test)
  add_dependencies(test_companion_transfer test_transfer_peer)
  target_link_libraries(test_companion_status PRIVATE otg_gui_support Qt6::Test)
  add_dependencies(test_companion_status test_peer)
  qt_add_executable(test_usb_auto_connect ../tests/test_usb_auto_connect.cpp)
  target_link_libraries(test_usb_auto_connect PRIVATE otg_gui_support Qt6::Test)
  add_dependencies(test_usb_auto_connect test_peer)
  target_link_libraries(test_gui PRIVATE otg_gui_support Qt6::Test)
  qt_add_resources(test_gui i18n_test_inputs
    PREFIX /i18n-test
    BASE "${PROJECT_SOURCE_DIR}"
    FILES translations/deskflow_otg_en.xml translations/deskflow_otg_zh_CN.xml
  )
  # Copy reference inputs as text: adding .cpp directly to a resource would mark
  # those shared sources HEADER_FILE_ONLY and remove them from other targets.
  set(i18n_test_sources)
  foreach(name main_window.cpp session.cpp process_utils.cpp deskflow_server.cpp connection_manager.cpp companion_app.cpp companion_transfer.cpp transfer_dialog.cpp)
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
  if(BUILD_BLE_WIN)
    add_dependencies(test_gui test_ble_win_control_peer)
    target_compile_definitions(test_gui PRIVATE
      OTG_TEST_NATIVE_BLE_PEER="$<TARGET_FILE:test_ble_win_control_peer>")
  endif()
  add_dependencies(test_window test_peer test_transfer_peer)
  if(MSVC)
    target_compile_options(test_gui PRIVATE /utf-8)
    target_compile_options(test_window PRIVATE /utf-8)
    target_compile_options(test_transfer_peer PRIVATE /utf-8)
    target_compile_options(test_companion_transfer PRIVATE /utf-8)
    set_property(TARGET test_gui test_window test_peer test_usb_auto_connect test_companion_status test_transfer_peer test_companion_transfer PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME gui_logic COMMAND test_gui)
  add_test(NAME companion_status COMMAND test_companion_status)
  add_test(NAME companion_transfer COMMAND test_companion_transfer)
  # QtTest otherwise sends plain-text diagnostics to the Windows debugger when
  # there is no console, leaving CTest/CI with an empty failure report.
  set_tests_properties(companion_transfer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1" TIMEOUT 60)
  add_test(NAME usb_auto_connect COMMAND test_usb_auto_connect)
  add_test(NAME gui_window COMMAND test_window)
  add_test(NAME gui_version COMMAND deskflow-otg-gui --version)
  add_test(NAME gui_runtime_files COMMAND "${CMAKE_COMMAND}"
    "-DDATA_DIR=${OTG_GUI_BUILD_DATA_DIR}"
    "-DGUI_DIR=$<TARGET_FILE_DIR:deskflow-otg-gui>"
    "-DHAVE_SCRCPY=$<OR:$<BOOL:${BUILD_AUDIO}>,$<BOOL:${BUILD_CLIPBOARD}>>"
    "-DSERVER=${SCRCPY_SERVER}" "-DVERSION=${SCRCPY_VERSION}"
    "-DADB_DIR=${ADB_BUNDLE_DIR}" "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
    "-DCOMPANION_APK=${companion_apk}"
    -P "${PROJECT_SOURCE_DIR}/tests/test_gui_runtime.cmake")
  set_tests_properties(gui_version PROPERTIES
    PASS_REGULAR_EXPRESSION "deskflow-otg-gui ${PROJECT_VERSION}")
  set_tests_properties(gui_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
