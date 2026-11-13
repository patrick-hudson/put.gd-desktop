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
# The putgd-ocr text recognition helper.
include(cmake/Ocr.cmake)
# The Backblaze B2 destination.
include(cmake/B2.cmake)
# The custom HTTP uploader destination.
include(cmake/Http.cmake)
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
  # The UI catalog, the interface benchmarks and the GUI tests.
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
# Region selection and X11 capture tests.
include(cmake/TestsRegionCapture.cmake)
# The Proton Drive destination.
include(cmake/Proton.cmake)
# The Backblaze B2 tests.
include(cmake/B2Tests.cmake)
# The custom HTTP uploader tests.
include(cmake/HttpTests.cmake)
# The scroll-capture stitcher, control loop and helper session tests.
include(cmake/TestsScroll.cmake)
# Package metadata.
include(cmake/NativeMetadata.cmake)
# Test labels and scheduling; last, once every test is registered.
include(cmake/TestPolicy.cmake)
