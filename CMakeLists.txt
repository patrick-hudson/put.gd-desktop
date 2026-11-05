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
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_gnome_bridge tests/integration/test_gnome_bridge.cpp)
    target_link_libraries(test_gnome_bridge PRIVATE putgd_gnome Qt6::Test)
    add_library(putgd_gnome_mime_fixture MODULE tests/fixtures/GnomeImageMimePlugin.cpp)
    target_link_libraries(putgd_gnome_mime_fixture PRIVATE Qt6::Gui)
    set_target_properties(putgd_gnome_mime_fixture PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture/imageformats")
    target_compile_definitions(test_gnome_bridge PRIVATE
      PUTGD_GNOME_MIME_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture")
    add_dependencies(test_gnome_bridge putgd-ui putgd_gnome_mime_fixture)
    # GNOME transfers decode PNG in the Core resident without image plugins.
    add_executable(test_gnome_image_decode tests/integration/test_gnome_image_decode.cpp)
    target_link_libraries(test_gnome_image_decode PRIVATE putgd_gnome Qt6::Test)
    target_compile_definitions(test_gnome_image_decode PRIVATE
      PUTGD_IMAGE_PLUGIN_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture")
    add_dependencies(test_gnome_image_decode putgd-ui putgd_gnome_mime_fixture)
    add_test(NAME gnome_image_decode COMMAND test_gnome_image_decode)
    set_tests_properties(gnome_image_decode PROPERTIES TIMEOUT 60)
    add_test(NAME gnome_bridge_contract COMMAND test_gnome_bridge)
    find_program(PUTGD_DBUS_RUN_SESSION NAMES dbus-run-session REQUIRED)
    add_test(NAME gnome_core_transport_integration COMMAND "${PUTGD_DBUS_RUN_SESSION}" -- $<TARGET_FILE:test_gnome_bridge> --core-adapter-transport)
    set_tests_properties(gnome_core_transport_integration PROPERTIES
      ENVIRONMENT "XDG_CURRENT_DESKTOP=GNOME;XDG_SESSION_TYPE=wayland;WAYLAND_DISPLAY=putgd-synthetic-wayland;DISPLAY=;QT_QPA_PLATFORM=wayland" TIMEOUT 60)
    # The extension is GJS code: use GNOME's existing runtime on its desktop,
    # never introduce Node.js as a host or guest test dependency.
    find_program(PUTGD_GJS gjs)
    if(PUTGD_GJS)
      add_test(NAME gnome_region_selection_contract COMMAND "${PUTGD_GJS}" -m "${PROJECT_SOURCE_DIR}/tests/desktop/test_gnome_region.mjs" "${PROJECT_SOURCE_DIR}/integrations/gnome/extension.js")
      set_tests_properties(gnome_region_selection_contract PROPERTIES TIMEOUT 30)
      add_test(NAME gnome_ui_ownership_contract COMMAND "${PUTGD_GJS}" -m "${PROJECT_SOURCE_DIR}/tests/desktop/test_gnome_ownership.mjs" "${PROJECT_SOURCE_DIR}/integrations/gnome/extension.js")
      set_tests_properties(gnome_ui_ownership_contract PROPERTIES TIMEOUT 30)
    else()
      message(STATUS "GNOME extension ownership contract runs in the GNOME lab with its native GJS runtime")
    endif()
    add_executable(test_kwin_transfer tests/integration/test_kwin_transfer.cpp)
    target_link_libraries(test_kwin_transfer PRIVATE putgd_kde Qt6::Test)
    add_test(NAME kwin_transfer_contract COMMAND test_kwin_transfer)
  endif()
  add_executable(test_recording_service tests/integration/test_recording_service.cpp)
  target_link_libraries(test_recording_service PRIVATE putgd_app Qt6::Test)
  target_include_directories(test_recording_service PRIVATE tests)
  target_compile_definitions(test_recording_service PRIVATE PUTGD_CLI_PATH="$<TARGET_FILE:putgd-cli>")
  add_dependencies(test_recording_service putgd-cli)
  add_test(NAME recording_service_integration COMMAND test_recording_service)
  set_tests_properties(recording_service_integration PROPERTIES TIMEOUT 120)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_record_helper_fixture tests/fixtures/record_helper_fixture.cpp)
    target_link_libraries(test_record_helper_fixture PRIVATE putgd_ui_control putgd_build_identity)
    add_executable(test_recording_helper tests/integration/test_recording_helper.cpp)
    target_link_libraries(test_recording_helper PRIVATE putgd_app putgd_ui_control Qt6::Test)
    target_include_directories(test_recording_helper PRIVATE tests)
    target_compile_definitions(test_recording_helper PRIVATE PUTGD_RECORD_FIXTURE="$<TARGET_FILE:test_record_helper_fixture>")
    add_dependencies(test_recording_helper test_record_helper_fixture)
    if(TARGET putgd-record)
      target_compile_definitions(test_recording_helper PRIVATE PUTGD_RECORD_HELPER="$<TARGET_FILE:putgd-record>")
      add_dependencies(test_recording_helper putgd-record)
    endif()
    add_test(NAME recording_helper_integration COMMAND test_recording_helper)
    set_tests_properties(recording_helper_integration PROPERTIES TIMEOUT 180)
  endif()
  add_executable(test_settings tests/unit/test_settings.cpp)
  target_include_directories(test_settings PRIVATE src)
  target_link_libraries(test_settings PRIVATE putgd_core Qt6::Test)
  add_test(NAME settings_contract COMMAND test_settings)
  # Persistent logging, retention, redaction canaries and unclean exits (#74).
  add_executable(test_logging tests/unit/test_logging.cpp)
  target_include_directories(test_logging PRIVATE src tests)
  target_link_libraries(test_logging PRIVATE putgd_app putgd_s3_runtime Qt6::Test)
  add_test(NAME logging_contract COMMAND test_logging)
  set_tests_properties(logging_contract PROPERTIES TIMEOUT 120)
  # Diagnostics packages: contents, redaction, size limit and the default folder (#74 §3).
  if(TARGET putgd_crash)
    add_library(test_crash_module MODULE tests/fixtures/crash_module.cpp)
    # Linked, so its constructor starts a thread before the executable's own run (#211).
    add_library(test_crash_early_library SHARED tests/fixtures/crash_early_library.cpp)
    target_link_libraries(test_crash_early_library PRIVATE Threads::Threads)
    add_executable(test_crash_handler tests/unit/test_crash_handler.cpp)
    target_include_directories(test_crash_handler PRIVATE src tests)
    target_link_libraries(test_crash_handler PRIVATE putgd_crash putgd_log test_crash_early_library Qt6::Test ${CMAKE_DL_LIBS})
    target_compile_definitions(test_crash_handler PRIVATE PUTGD_CRASH_MODULE="$<TARGET_FILE:test_crash_module>"
                               PUTGD_CRASH_EARLY_LIBRARY="$<TARGET_FILE:test_crash_early_library>")
    add_dependencies(test_crash_handler test_crash_module)
    add_test(NAME crash_handler_contract COMMAND test_crash_handler)
  endif()
  add_executable(test_diagnostics_upload tests/unit/test_diagnostics_upload.cpp)
  target_include_directories(test_diagnostics_upload PRIVATE src tests)
  target_link_libraries(test_diagnostics_upload PRIVATE putgd_app Qt6::Test)
  add_test(NAME diagnostics_upload_contract COMMAND test_diagnostics_upload)
  add_executable(test_diagnostics_bundle tests/unit/test_diagnostics_bundle.cpp)
  target_include_directories(test_diagnostics_bundle PRIVATE src tests)
  target_link_libraries(test_diagnostics_bundle PRIVATE putgd_app Qt6::Network Qt6::Test)
  target_compile_definitions(test_diagnostics_bundle PRIVATE PUTGD_CLI_PATH="$<TARGET_FILE:putgd-cli>")
  add_dependencies(test_diagnostics_bundle putgd-cli)
  add_test(NAME diagnostics_bundle_contract COMMAND test_diagnostics_bundle)
  set_tests_properties(diagnostics_bundle_contract PROPERTIES TIMEOUT 180)
  set_tests_properties(settings_contract PROPERTIES TIMEOUT 30)
  add_test(NAME json_ref_lifetime_contract COMMAND "${CMAKE_COMMAND}"
    "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/json_ref_lifetime_contract.cmake")
  set_tests_properties(json_ref_lifetime_contract PROPERTIES TIMEOUT 30)
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
