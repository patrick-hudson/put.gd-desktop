cmake_minimum_required(VERSION 3.24)
project(putgd LANGUAGES C CXX)

include(CTest)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(CheckPIESupported)
  check_pie_supported(LANGUAGES C CXX OUTPUT_VARIABLE putgd_pie_error)
  if(NOT CMAKE_C_LINK_PIE_SUPPORTED OR NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
    message(FATAL_ERROR "Linux executables require PIE: ${putgd_pie_error}")
  endif()
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  add_link_options("LINKER:-z,relro" "LINKER:-z,now")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Installation prefix" FORCE)
endif()
include(GNUInstallDirs)
include(cmake/BuildVersion.cmake)
include(cmake/Dependencies.cmake)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# The build, in order: each file below uses the targets and variables of the ones before it.
# Build options: warnings, sanitizers, debug splitting and the crash handler.
include(cmake/BuildOptions.cmake)
# The AWS SDK runtime library and the probe for its Curl transport.
include(cmake/S3Runtime.cmake)
# The log, core and credential libraries, and which package a build is for.
include(cmake/Core.cmake)
# The S3 destination: its wire protocol, provider, SDK module and worker.
include(cmake/S3.cmake)
# The application library and the files it compiles in.
include(cmake/App.cmake)
# The X11, GNOME and KDE/Wayland desktop libraries.
include(cmake/Platform.cmake)
# The interface libraries, down to putgd_ui_application.
include(cmake/Ui.cmake)
# The product executables and their install rules, apart from the helpers with their own file.
include(cmake/Executables.cmake)
# The putgd-record helper and the tests built from its sources.
include(cmake/Recording.cmake)
# The Backblaze B2 destination.
include(cmake/B2.cmake)
# The crash handler and debug splitting for every product binary.
include(cmake/ProductBinaries.cmake)
# The file-manager menu plugins.
include(cmake/FileManagerPlugins.cmake)
# Desktop integration and package contents.
add_subdirectory(packaging/linux)
# The opt-in benchmark driver and putgd_xvfb_display, which every Xvfb test calls.
include(cmake/TestSupport.cmake)
# The tests. Their order sets the Xvfb display each one starts its search from.
if(BUILD_TESTING)
  # KDE, X11 and native session tests, and the version contract.
  include(cmake/TestsPlatform.cmake)
  # Helper-process, editor and IPC tests.
  include(cmake/TestsHelpers.cmake)
  # The UI catalog, its references, the interface benchmarks and the GUI tests.
  include(cmake/TestsGui.cmake)
  # Update, release link and small core unit tests.
  include(cmake/TestsUpdate.cmake)
  # Tests on a private bus and display: tray, resident, file dialog, host open, keychain backend.
  include(cmake/TestsNativeSession.cmake)
  # Keychain, S3, worker, SDK and build contract tests, and the opt-in upload probes.
  include(cmake/TestsS3.cmake)
  # The payload, history, operations, storage and CLI contract loops.
  include(cmake/TestsContracts.cmake)
  # GNOME bridge, extension and KWin transfer tests.
  include(cmake/TestsGnome.cmake)
  # The recording service and its helper protocol tests.
  include(cmake/TestsRecordingService.cmake)
  # Settings, logging, crash handler and diagnostics tests.
  include(cmake/TestsDiagnostics.cmake)
endif()
# History and Settings views, login startup, file uploads, local IPC, and the Qt Network links.
include(cmake/TestsHistoryUploads.cmake)
if(BUILD_TESTING)
  add_executable(test_region_selection tests/unit/test_region_selection.cpp)
  target_include_directories(test_region_selection PRIVATE src)
  target_link_libraries(test_region_selection PRIVATE putgd_build_options putgd_visual Qt6::Widgets Qt6::Test)
  add_test(NAME region_selection_contract COMMAND test_region_selection)
  # The hint placement measures text, so the test needs a (headless) GUI platform.
  set_tests_properties(region_selection_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(BUILD_TESTING AND TARGET putgd_kde_region_process)
  # Exercise the production QWidget without a native layer role. Real KWin
  # keyboard focus and transparent-frame dismissal remain desktop gates.
  add_executable(test_kde_region_overlay tests/gui/test_kde_region_overlay.cpp src/platform/kde/KdeRegionOverlay.cpp)
  target_link_libraries(test_kde_region_overlay PRIVATE putgd_kde_region_process Qt6::Widgets Qt6::Test)
  add_test(NAME kde_region_overlay COMMAND test_kde_region_overlay)
  set_tests_properties(kde_region_overlay PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
  add_test(NAME kde_region_overlay_multihead COMMAND test_kde_region_overlay surfacesStayOnTheirOutputs)
  set_tests_properties(kde_region_overlay_multihead PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen:configfile=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/offscreen-kde-multihead.json" TIMEOUT 30)
endif()

if(BUILD_TESTING AND TARGET putgd_x11)
  add_executable(test_native_region_overlay tests/desktop/x11/test_native_region_overlay.cpp)
  target_link_libraries(test_native_region_overlay PRIVATE putgd_x11 putgd_capture_cursor Qt6::Test PkgConfig::PUTGD_XTEST PkgConfig::PUTGD_XCB)
  putgd_xvfb_display(xvfb)
  add_test(NAME native_region_overlay COMMAND dbus-run-session -- ${PUTGD_XVFB_RUN} ${xvfb} -s "-screen 0 1280x800x24 -noreset" $<TARGET_FILE:test_native_region_overlay>)
  set_tests_properties(native_region_overlay PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;XDG_SESSION_TYPE=x11;WAYLAND_DISPLAY=" TIMEOUT 60)
endif()

if(BUILD_TESTING AND TARGET putgd-capture)
  add_executable(test_capture_preparation tests/desktop/x11/test_capture_preparation.cpp)
  target_link_libraries(test_capture_preparation PRIVATE putgd_x11 putgd_x11_native_snapshot putgd_capture_control putgd_capture_cursor putgd_build_identity Qt6::Test Qt6::Gui Qt6::Network PkgConfig::PUTGD_XCB PkgConfig::PUTGD_XCURSOR PkgConfig::PUTGD_HOLD_XTEST)
  target_compile_definitions(test_capture_preparation PRIVATE PUTGD_REAL_CAPTURE_HELPER="$<TARGET_FILE:putgd-capture>")
  add_dependencies(test_capture_preparation putgd-capture)
  putgd_xvfb_display(xvfb)
  add_test(NAME capture_preparation_x11 COMMAND ${PUTGD_XVFB_RUN} ${xvfb} -s "-screen 0 1280x800x24 -noreset" $<TARGET_FILE:test_capture_preparation>)
  set_tests_properties(capture_preparation_x11 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;XDG_SESSION_TYPE=x11;WAYLAND_DISPLAY=" TIMEOUT 30)
endif()
# The Proton Drive destination.
include(cmake/Proton.cmake)
# The Backblaze B2 tests.
include(cmake/B2Tests.cmake)
# Package metadata.
include(cmake/NativeMetadata.cmake)
# Test labels and scheduling; last, once every test is registered.
include(cmake/TestPolicy.cmake)
