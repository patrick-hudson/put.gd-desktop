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
if(BUILD_TESTING)
  foreach(view settings history_view)
    add_executable(test_gui_${view} tests/gui/test_${view}.cpp)
    target_include_directories(test_gui_${view} PRIVATE tests)
    target_link_libraries(test_gui_${view} PRIVATE putgd_ui putgd_app putgd_s3 Qt6::Test)
    add_test(NAME gui_${view} COMMAND test_gui_${view})
    set_tests_properties(gui_${view} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
  endforeach()
  # Delayed saves, setup polling and theme/scale cases expand this functional
  # suite; leave time for a complete run on the shared builders.
  set_tests_properties(gui_settings PROPERTIES TIMEOUT 180)
  # Copy image decodes the retained original in the putgd-ui image worker (#284).
  add_dependencies(test_gui_history_view putgd-ui)
  set_tests_properties(gui_history_view PROPERTIES TIMEOUT 120)
  # The History layout at 150% text: gallery cards, columns and the compact details view.
  add_test(NAME gui_history_view_text150 COMMAND test_gui_history_view historyHeadingKeepsItsWordsBeforeSearchYields compactFailureDetailIsNeverShortenedBesideItsActions historyToolbarKeepsItsControlsTogether rowsReserveOnlyTheirOwnPrimaryAction remoteGroupsUseTheProductionQueryAndRestoreExactStates inspectorDeduplicatesNamesAndOutcomes galleryCardsReserveTextBeforeTheirActions columnHeadingsMatchRowColumns compactLayoutOpensDetailsOverTheList selectedRowLeavesItsPrimaryActionToTheInspector rowActionsNeverCoverFilenameOrStatus expandedFiltersKeepTwoWholeResults galleryInspectorUsesTheVisibleCardAndKeepsItsActionSpace localAvailabilityFollowsTheFileWithoutChangingTheReceipt compactFailureWrapsBeforeTheActionFooter compactRowsSpendTheirWidthOnTitles compactDetailsKeepFactsAndPrimaryOnTheFirstScreen)
  set_tests_properties(gui_history_view_text150 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PUTGD_TEXT_SCALE=1.5" TIMEOUT 120)
endif()

if(BUILD_TESTING)
 add_executable(test_login_startup tests/unit/test_login_startup.cpp)
 target_link_libraries(test_login_startup PRIVATE putgd_app Qt6::Test)
 add_test(NAME login_startup_contract COMMAND test_login_startup)
 # #1105: per-user file-manager entries, the COSMIC merge and the generated scripts.
 add_executable(test_file_manager_integration tests/unit/test_file_manager_integration.cpp)
 target_link_libraries(test_file_manager_integration PRIVATE putgd_app Qt6::Test)
 add_test(NAME file_manager_integration COMMAND test_file_manager_integration)
 set_tests_properties(file_manager_integration PROPERTIES TIMEOUT 120)
 # #1105: streaming ZIP64 folder archives; PUTGD_FOLDER_ZIP_LARGE=1 adds the >4 GiB and >65,535-entry cases.
 add_executable(test_folder_zip tests/unit/test_folder_zip.cpp)
 target_link_libraries(test_folder_zip PRIVATE putgd_core ZLIB::ZLIB Qt6::Test)
 add_test(NAME folder_zip COMMAND test_folder_zip)
 # #1105: upload.files checks, routing and the --upload client's pure parts.
 add_executable(test_file_uploads tests/unit/test_file_uploads.cpp)
 target_include_directories(test_file_uploads PRIVATE src tests)
 target_link_libraries(test_file_uploads PRIVATE putgd_app putgd_upload_client Qt6::Test)
 add_test(NAME file_uploads COMMAND test_file_uploads)
 # #490: a resident ends with its login session; fake logind.
 add_executable(test_resident_session tests/unit/test_resident_session.cpp)
 target_include_directories(test_resident_session PRIVATE tests)
 target_link_libraries(test_resident_session PRIVATE putgd_app Qt6::Test)
 add_test(NAME resident_session_contract COMMAND test_resident_session)
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(test_local_ipc tests/integration/test_local_ipc.cpp)
  target_include_directories(test_local_ipc PRIVATE tests)
  target_link_libraries(test_local_ipc PRIVATE putgd_app Qt6::Test Qt6::Network)
  add_test(NAME local_ipc_contract COMMAND test_local_ipc)
  set_tests_properties(local_ipc_contract PROPERTIES TIMEOUT 60)
endif()
if(BUILD_TESTING)
  add_executable(test_ip_address tests/unit/test_ip_address.cpp)
  target_link_libraries(test_ip_address PRIVATE putgd_s3 Qt6::Test Qt6::Network)
  add_test(NAME ip_address_contract COMMAND test_ip_address)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(putgd_app PRIVATE Qt6::Network)
  target_link_libraries(putgd_s3 PRIVATE Qt6::Network)
endif()
if(BUILD_TESTING)
  foreach(network_fixture test_cli test_s3 test_s3_multipart test_s3_delete test_collision_policy test_s3_module)
    target_link_libraries(${network_fixture} PRIVATE Qt6::Network)
  endforeach()
endif()
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
