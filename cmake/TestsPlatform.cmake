# KDE, X11 and native session tests, and the version contract.
# Included inside if(BUILD_TESTING) in the top-level CMakeLists.txt.
  if(TARGET putgd_wayland_region)
    add_executable(test_wayland_region_control tests/unit/test_wayland_region_control.cpp)
    target_link_libraries(test_wayland_region_control PRIVATE putgd_wayland_region Qt6::Test)
    add_test(NAME wayland_region_control COMMAND test_wayland_region_control)
  endif()
  add_executable(test_native_session tests/unit/test_native_session.cpp)
  target_include_directories(test_native_session PRIVATE src)
  target_link_libraries(test_native_session PRIVATE putgd_build_options Qt6::Core Qt6::Test)
  add_test(NAME native_session_guards COMMAND test_native_session)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_native_tray tests/integration/test_native_tray.cpp)
    target_include_directories(test_native_tray PRIVATE tests)
    target_link_libraries(test_native_tray PRIVATE putgd_tray Qt6::Test Qt6::DBus)
    # A bus that can activate nothing: links and folders now ask the XDG portal
    # first (#326), and a real one would open the host's browser or file manager.
    add_test(NAME native_tray_integration COMMAND dbus-run-session --config-file=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dbus-session-isolated.conf -- $<TARGET_FILE:test_native_tray>)
    set_tests_properties(native_tray_integration PROPERTIES TIMEOUT 90)
  endif()
  if(TARGET putgd_x11_core)
    add_executable(test_capture_control tests/integration/test_capture_control.cpp)
    target_link_libraries(test_capture_control PRIVATE putgd_capture_control putgd_x11_native_snapshot Qt6::Test)
    add_test(NAME capture_control_integration COMMAND test_capture_control)
    add_executable(test_capture_helper_fixture tests/fixtures/capture_helper_fixture.cpp)
    target_link_libraries(test_capture_helper_fixture PRIVATE putgd_capture_control putgd_core putgd_build_identity putgd_ui_control)
    add_executable(test_capture_process tests/integration/test_capture_process.cpp)
    pkg_check_modules(PUTGD_HOLD_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)
    target_link_libraries(test_capture_process PRIVATE putgd_x11_core Qt6::Test PkgConfig::PUTGD_XCB PkgConfig::PUTGD_HOLD_XTEST)
    target_compile_definitions(test_capture_process PRIVATE PUTGD_CAPTURE_FIXTURE="$<TARGET_FILE:test_capture_helper_fixture>")
    add_dependencies(test_capture_process test_capture_helper_fixture)
    putgd_xvfb_display(xvfb)
    add_test(NAME capture_process_integration COMMAND xvfb-run ${xvfb} -s "-screen 0 1280x800x24 -noreset" $<TARGET_FILE:test_capture_process>)
    set_tests_properties(capture_process_integration PROPERTIES ENVIRONMENT "QT_IMAGEIO_MAXALLOC=1;XDG_SESSION_TYPE=x11;WAYLAND_DISPLAY=" TIMEOUT 90)
    add_executable(test_x11_core_clipboard tests/integration/test_x11_core_clipboard.cpp)
    target_link_libraries(test_x11_core_clipboard PRIVATE putgd_x11_listeners Qt6::Test PkgConfig::PUTGD_XCB)
    putgd_xvfb_display(xvfb)
    add_test(NAME x11_core_clipboard COMMAND dbus-run-session -- xvfb-run ${xvfb} -s "-screen 0 1280x800x24 -noreset" $<TARGET_FILE:test_x11_core_clipboard>)
    # Non-PNG images and image type discovery run in putgd-ui's image job.
    add_executable(test_x11_clipboard_import tests/integration/test_x11_clipboard_import.cpp)
    target_link_libraries(test_x11_clipboard_import PRIVATE putgd_x11_listeners Qt6::Test PkgConfig::PUTGD_XCB)
    add_library(putgd_slow_image_fixture MODULE tests/fixtures/SlowImagePlugin.cpp)
    target_link_libraries(putgd_slow_image_fixture PRIVATE Qt6::Gui)
    set_target_properties(putgd_slow_image_fixture PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/slow-image-fixture/imageformats")
    target_compile_definitions(test_x11_clipboard_import PRIVATE
      PUTGD_IMAGE_PLUGIN_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture"
      PUTGD_SLOW_PLUGIN_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/slow-image-fixture")
    add_dependencies(test_x11_clipboard_import putgd-ui putgd_gnome_mime_fixture putgd_slow_image_fixture)
    putgd_xvfb_display(xvfb)
    add_test(NAME x11_clipboard_import COMMAND dbus-run-session -- xvfb-run ${xvfb} -s "-screen 0 1280x800x24 -noreset" $<TARGET_FILE:test_x11_clipboard_import>)
    # Verified PNG captures decode in the resident without image plugins.
    add_executable(test_x11_capture_decode tests/integration/test_x11_capture_decode.cpp)
    target_link_libraries(test_x11_capture_decode PRIVATE putgd_x11_core putgd_x11 Qt6::Test Qt6::DBus)
    target_compile_definitions(test_x11_capture_decode PRIVATE
      PUTGD_CAPTURE_FIXTURE="$<TARGET_FILE:test_capture_helper_fixture>"
      PUTGD_IMAGE_PLUGIN_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture")
    add_dependencies(test_x11_capture_decode test_capture_helper_fixture putgd_gnome_mime_fixture)
    putgd_xvfb_display(xvfb)
    add_test(NAME x11_capture_decode COMMAND dbus-run-session -- xvfb-run ${xvfb} -s "-screen 0 1280x800x24 -noreset" $<TARGET_FILE:test_x11_capture_decode>)
    add_executable(test_x11_native_snapshot tests/desktop/x11/test_native_snapshot.cpp)
    target_link_libraries(test_x11_native_snapshot PRIVATE putgd_x11_native_snapshot Qt6::Test PkgConfig::PUTGD_XCB)
    putgd_xvfb_display(xvfb)
    add_test(NAME x11_native_snapshot COMMAND xvfb-run ${xvfb} -s "-screen 0 1280x800x24 -noreset" $<TARGET_FILE:test_x11_native_snapshot>)
    set_tests_properties(x11_core_clipboard x11_clipboard_import x11_capture_decode x11_native_snapshot PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=xcb;XDG_SESSION_TYPE=x11;WAYLAND_DISPLAY=" TIMEOUT 90)
  endif()
  add_test(NAME version_contract COMMAND "${CMAKE_COMMAND}"
    "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/Testing/version-contract-$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/version_contract.cmake")
  set_tests_properties(version_contract PROPERTIES TIMEOUT 90)
  if(TARGET putgd_kde)
    add_executable(test_kde_shortcut_normalization tests/unit/test_kde_shortcut_normalization.cpp)
    target_link_libraries(test_kde_shortcut_normalization PRIVATE putgd_kde Qt6::Test)
    add_test(NAME kde_shortcut_normalization COMMAND test_kde_shortcut_normalization)
    add_executable(test_kde_shortcuts tests/integration/test_kde_shortcuts.cpp)
    target_link_libraries(test_kde_shortcuts PRIVATE putgd_kde Qt6::Test)
    add_test(NAME kde_shortcuts_integration COMMAND dbus-run-session -- $<TARGET_FILE:test_kde_shortcuts>)
    add_executable(test_kwin_windows tests/integration/test_kwin_windows.cpp)
    target_link_libraries(test_kwin_windows PRIVATE putgd_kde Qt6::Test Qt6::DBus)
    add_test(NAME kwin_windows_integration COMMAND dbus-run-session -- $<TARGET_FILE:test_kwin_windows>)
    add_executable(test_wayland_keyboard_map tests/unit/test_wayland_keyboard_map.cpp)
    target_link_libraries(test_wayland_keyboard_map PRIVATE putgd_kde Qt6::Test)
    add_test(NAME wayland_keyboard_map COMMAND test_wayland_keyboard_map)
    add_executable(test_shortcut_keyboard tests/unit/test_shortcut_keyboard.cpp)
    target_link_libraries(test_shortcut_keyboard PRIVATE putgd_shortcut_keyboard Qt6::Test PkgConfig::PUTGD_XKBCOMMON)
    add_test(NAME shortcut_keyboard COMMAND test_shortcut_keyboard)
    pkg_check_modules(PUTGD_WAYLAND_TEST_SERVER REQUIRED IMPORTED_TARGET wayland-server)
    add_custom_command(OUTPUT "${protocol_dir}/xdg-output-unstable-v1-server-protocol.h"
      COMMAND "${PUTGD_WAYLAND_SCANNER}" server-header "${xdg_output_protocol}" "${protocol_dir}/xdg-output-unstable-v1-server-protocol.h"
      DEPENDS "${xdg_output_protocol}" VERBATIM)
    foreach(protocol_name ext-data-control-v1 wlr-data-control-unstable-v1)
      add_custom_command(OUTPUT "${protocol_dir}/${protocol_name}-server-protocol.h"
        COMMAND "${PUTGD_WAYLAND_SCANNER}" server-header "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/wayland/protocols/${protocol_name}.xml" "${protocol_dir}/${protocol_name}-server-protocol.h"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/wayland/protocols/${protocol_name}.xml" VERBATIM)
    endforeach()
    add_executable(test_kde_ownership tests/integration/test_kde_ownership.cpp
      "${protocol_dir}/xdg-output-unstable-v1-server-protocol.h"
      "${protocol_dir}/ext-data-control-v1-server-protocol.h" "${protocol_dir}/wlr-data-control-unstable-v1-server-protocol.h")
    target_include_directories(test_kde_ownership PRIVATE "${protocol_dir}")
    target_link_libraries(test_kde_ownership PRIVATE putgd_kde Qt6::Test PkgConfig::PUTGD_WAYLAND_TEST_SERVER)
    if(TARGET putgd-record)
      target_compile_definitions(test_kde_ownership PRIVATE PUTGD_RECORD_DESKTOP_ENTRY="${CMAKE_BINARY_DIR}/packaging/linux/putgd-record.desktop")
    endif()
    add_test(NAME kde_ownership_integration COMMAND dbus-run-session -- $<TARGET_FILE:test_kde_ownership>)
    set_tests_properties(kde_ownership_integration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_custom_command(OUTPUT "${protocol_dir}/ext-data-control-v1-server-protocol.h"
      COMMAND "${PUTGD_WAYLAND_SCANNER}" server-header "${data_control_protocol}" "${protocol_dir}/ext-data-control-v1-server-protocol.h"
      DEPENDS "${data_control_protocol}" VERBATIM)
    # PNG offers decode in the Core resident without image plugins.
    add_executable(test_kde_clipboard_import tests/integration/test_kde_clipboard_import.cpp
      "${protocol_dir}/ext-data-control-v1-server-protocol.h")
    target_include_directories(test_kde_clipboard_import PRIVATE "${protocol_dir}")
    target_link_libraries(test_kde_clipboard_import PRIVATE putgd_kde Qt6::Test PkgConfig::PUTGD_WAYLAND_TEST_SERVER)
    target_compile_definitions(test_kde_clipboard_import PRIVATE
      PUTGD_IMAGE_PLUGIN_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture")
    add_dependencies(test_kde_clipboard_import putgd-ui putgd_gnome_mime_fixture)
    add_test(NAME kde_clipboard_import COMMAND test_kde_clipboard_import)
    set_tests_properties(kde_clipboard_import PROPERTIES TIMEOUT 60)
    add_executable(test_wayland_data_control_source tests/integration/test_wayland_data_control_source.cpp
      "${protocol_dir}/ext-data-control-v1-server-protocol.h")
    target_include_directories(test_wayland_data_control_source PRIVATE "${protocol_dir}")
    target_link_libraries(test_wayland_data_control_source PRIVATE putgd_kde Qt6::Test PkgConfig::PUTGD_WAYLAND_TEST_SERVER)
    add_test(NAME wayland_data_control_source COMMAND test_wayland_data_control_source)
    set_tests_properties(wayland_data_control_source PROPERTIES TIMEOUT 60)
  endif()
# The Budgie adapter and shared Wayland capture against a real headless labwc
# (#1275). It skips where labwc is not installed; the Ubuntu 26.04 CI leg has it.
if(BUILD_TESTING AND TARGET putgd_budgie)
  set(session_lock_xml "${PUTGD_WAYLAND_PROTOCOLS_DIR}/staging/ext-session-lock/ext-session-lock-v1.xml")
  add_custom_command(OUTPUT "${protocol_dir}/ext-session-lock-v1-client-protocol.h" "${protocol_dir}/ext-session-lock-v1-protocol.c"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${session_lock_xml}" "${protocol_dir}/ext-session-lock-v1-client-protocol.h"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${session_lock_xml}" "${protocol_dir}/ext-session-lock-v1-protocol.c"
    DEPENDS "${session_lock_xml}" VERBATIM)
  add_executable(putgd-session-lock-fixture tests/fixtures/session_lock.c
    "${protocol_dir}/ext-session-lock-v1-client-protocol.h" "${protocol_dir}/ext-session-lock-v1-protocol.c")
  target_include_directories(putgd-session-lock-fixture PRIVATE "${protocol_dir}")
  target_link_libraries(putgd-session-lock-fixture PRIVATE PkgConfig::PUTGD_WAYLAND)
  add_executable(test_budgie_labwc tests/integration/test_budgie_labwc.cpp)
  add_dependencies(test_budgie_labwc putgd-session-lock-fixture)
  target_link_libraries(test_budgie_labwc PRIVATE putgd_budgie Qt6::Test)
  target_compile_definitions(test_budgie_labwc PRIVATE PUTGD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(TARGET putgd-region)
    add_dependencies(test_budgie_labwc putgd-region)
  endif()
  if(TARGET putgd-clipboard-keeper)
    add_dependencies(test_budgie_labwc putgd-clipboard-keeper)
  endif()
  if(PUTGD_LAYER_SHELL_REGION)
    add_executable(putgd-labwc-fixture tests/fixtures/labwc_fixture.cpp)
    target_link_libraries(putgd-labwc-fixture PRIVATE putgd_build_options Qt6::Widgets LayerShellQt::Interface)
    add_dependencies(test_budgie_labwc putgd-labwc-fixture)
  endif()
  if(TARGET putgd-record)
    # The recorder's image-copy source on headless labwc: rotated outputs record upright (#1275 review).
    # Here, after the test target exists (Recording.cmake is included first).
    target_sources(test_budgie_labwc PRIVATE src/record/ImageCopySource.cpp src/record/DebugLog.cpp)
    target_include_directories(test_budgie_labwc PRIVATE src "${protocol_dir}")
    target_compile_definitions(test_budgie_labwc PRIVATE PUTGD_TEST_IMAGE_COPY_SOURCE=1)
    target_link_libraries(test_budgie_labwc PRIVATE PkgConfig::PUTGD_GST PkgConfig::PUTGD_WAYLAND)
  endif()
  # The scroll capture's virtual pointer against the real compositor (#1376): a wheel notch reaches a window.
  target_sources(test_budgie_labwc PRIVATE src/platform/wayland/VirtualPointer.cpp)
  target_include_directories(test_budgie_labwc PRIVATE src "${protocol_dir}")
  target_link_libraries(test_budgie_labwc PRIVATE PkgConfig::PUTGD_WAYLAND)
  add_test(NAME budgie_labwc_integration COMMAND dbus-run-session -- $<TARGET_FILE:test_budgie_labwc>)
  set_tests_properties(budgie_labwc_integration PROPERTIES TIMEOUT 120)
  # The labwc bridge's gsettings sequencing against a fake gsettings; needs no labwc.
  add_executable(test_labwc_bridge tests/integration/test_labwc_bridge.cpp)
  target_link_libraries(test_labwc_bridge PRIVATE putgd_budgie Qt6::Test)
  add_test(NAME labwc_bridge_contract COMMAND dbus-run-session -- $<TARGET_FILE:test_labwc_bridge>)
  set_tests_properties(labwc_bridge_contract PROPERTIES TIMEOUT 120)
  # GlobalShortcuts portal backend against a scripted portal on a private bus.
  add_executable(test_portal_global_shortcuts tests/integration/test_portal_global_shortcuts.cpp)
  target_link_libraries(test_portal_global_shortcuts PRIVATE putgd_wayland Qt6::DBus Qt6::Test)
  add_test(NAME portal_global_shortcuts_contract COMMAND dbus-run-session -- $<TARGET_FILE:test_portal_global_shortcuts>)
  set_tests_properties(portal_global_shortcuts_contract PROPERTIES TIMEOUT 60)
endif()
# Hyprland (#1275): key/bind contracts against an Omarchy 4.5 bind list, the
# permission refusal and rule text, and the shortcut backend against a fake
# Hyprland IPC server. The real compositor is covered by the Omarchy golden.
if(BUILD_TESTING AND TARGET putgd_hyprland)
  add_executable(test_hyprland tests/unit/test_hyprland.cpp)
  target_include_directories(test_hyprland PRIVATE tests)
  target_link_libraries(test_hyprland PRIVATE putgd_hyprland Qt6::Test)
  target_compile_definitions(test_hyprland PRIVATE PUTGD_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME hyprland_contracts COMMAND test_hyprland)
  set_tests_properties(hyprland_contracts PROPERTIES TIMEOUT 120)
endif()
