# put.gd for Windows: a native Win32 implementation of the resident, its
# transient helpers and the CLI. No Qt. Configure this directory on its own:
#
#   cmake -S src/windows -B build/windows -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-w64-mingw32.cmake \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/windows
#
# or natively with MSVC/clang-cl on Windows. The portable core (JSON, settings,
# naming, operation records) also builds and tests on Linux with
# -DPUTGD_WINDOWS_PORTABLE_TESTS=ON, which is what CI runs.
#
# Process model (mirrors the Linux resident/helper split, see docs/memory-strategy.md):
#   putgd.exe                 resident: tray, hotkeys, clipboard owner, journal,
#                             named-pipe session server; spawns the helpers below
#   putgd.exe --capture       live region/window/desktop selector + GDI capture + WIC PNG
#   putgd.exe --image-job     decode/normalize clipboard or file images, previews (WIC)
#   putgd.exe --s3-worker     one S3 request: SigV4 over WinHTTP, exits afterwards
#   putgd.exe --ui VIEW       Settings/Destinations/History/Activity/About (common controls)
#   putgd.exe --edit-image    Capture region & edit canvas (GDI+)
#   putgd-cli.exe             JSON session client over the named pipe
cmake_minimum_required(VERSION 3.22)
project(putgd-windows LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(PUTGD_WINDOWS_PORTABLE_TESTS "Build the portable core and its tests on this host" ${UNIX})

# Build identity: the repository's calendar version (docs/versioning.md, YY.MDD.N
# from the commit date and count) through the same generator the Linux build uses.
set(PUTGD_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(PUTGD_RELEASE_TAG "" CACHE STRING "Exact clean Git release tag: vYY.MDD.N or vYY.MDD.N-nightly")
set(_identity_dir "${CMAKE_CURRENT_BINARY_DIR}/build-identity")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PUTGD_SOURCE_ROOT}" "-DOUTPUT_DIR=${_identity_dir}" "-DRELEASE_TAG=${PUTGD_RELEASE_TAG}"
  "-DSUPPLIED_COMMIT=" "-DSUPPLIED_TREE=" "-DSUPPLIED_COUNT=" "-DSUPPLIED_TIME=" -P "${PUTGD_SOURCE_ROOT}/cmake/GenerateBuildInfo.cmake" RESULT_VARIABLE _identity_result)
if(NOT _identity_result EQUAL 0)
  message(FATAL_ERROR "Cannot generate the build identity")
endif()
file(READ "${_identity_dir}/build-info.json" _manifest)
string(JSON PUTGD_VERSION GET "${_manifest}" version)
string(JSON _base GET "${_manifest}" baseVersion)
string(JSON _commit GET "${_manifest}" gitCommit)
string(JSON _count GET "${_manifest}" commitCount)
string(JSON _dirty GET "${_manifest}" dirty)
configure_file(common/BuildInfo.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/common/BuildInfo.h" @ONLY)
# VERSIONINFO of both executables and the MSI version (#396), from the same manifest.
include(cmake/WindowsVersion.cmake)
putgd_windows_version("${PUTGD_VERSION}" PUTGD_WINDOWS)
configure_file(common/VersionResource.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/common/VersionResource.h" @ONLY)
# Read by src/windows/packaging/package.py, which builds the zip and the MSI.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/windows-version.json.tmp"
  "{\"version\": \"${PUTGD_VERSION}\", \"fileVersion\": \"${PUTGD_WINDOWS_FILE_VERSION_TEXT}\", \"msiVersion\": \"${PUTGD_WINDOWS_MSI_VERSION}\"}\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/windows-version.json.tmp" "${CMAKE_CURRENT_BINARY_DIR}/windows-version.json" COPYONLY)

add_library(putgd_win_options INTERFACE)
target_include_directories(putgd_win_options INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(MSVC)
  # The warning set of the mingw build: no unused-parameter warnings (-Wno-unused-parameter)
  # and no shadowing warnings (GCC has no -Wshadow in -Wall -Wextra).
  target_compile_options(putgd_win_options INTERFACE /W4 /utf-8 /EHsc /permissive- /wd4100 /wd4456 /wd4457)
  target_compile_definitions(putgd_win_options INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(putgd_win_options INTERFACE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
if(WIN32)
  target_compile_definitions(putgd_win_options INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006)
  if(MINGW)
    # One self-contained executable: no libstdc++/libgcc/winpthread DLLs to ship,
    # and nothing to load at resident start beyond system DLLs.
    target_link_options(putgd_win_options INTERFACE -static -static-libgcc -static-libstdc++ -municode)
    target_compile_options(putgd_win_options INTERFACE -municode)
  endif()
endif()

# Portable core: no Windows headers. Tested on Linux and used by every executable.
add_library(putgd_win_core STATIC
  common/Json.cpp common/Text.cpp common/Shortcut.cpp common/Sha256.cpp
  core/Settings.cpp core/Naming.cpp core/Operation.cpp core/SettingsPatch.cpp core/S3Settings.cpp core/SanitizedPng.cpp core/ProtonSettings.cpp
  common/Blake2b.cpp common/Ed25519.cpp common/Minisign.cpp core/UpdatePlan.cpp
  s3/SigV4.cpp)
target_link_libraries(putgd_win_core PUBLIC putgd_win_options)

if(PUTGD_WINDOWS_PORTABLE_TESTS)
  enable_testing()
  add_executable(test_win_core tests/test_core.cpp)
  target_link_libraries(test_win_core PRIVATE putgd_win_core)
  add_test(NAME windows_core_contract COMMAND test_win_core)
  add_executable(test_win_sigv4 tests/test_sigv4.cpp)
  target_link_libraries(test_win_sigv4 PRIVATE putgd_win_core)
  add_test(NAME windows_sigv4 COMMAND test_win_sigv4)
  add_executable(test_win_editor tests/test_editor.cpp editor/EditDocument.cpp editor/EditorCommands.cpp editor/Export.cpp)
  target_link_libraries(test_win_editor PRIVATE putgd_win_core)
  add_test(NAME windows_editor_contract COMMAND test_win_editor)
  add_executable(test_win_minisign tests/test_minisign.cpp)
  target_link_libraries(test_win_minisign PRIVATE putgd_win_core)
  add_executable(test_win_update tests/test_update.cpp)
  target_link_libraries(test_win_update PRIVATE putgd_win_core)
  add_test(NAME windows_update_plan COMMAND test_win_update)
  add_test(NAME windows_minisign COMMAND test_win_minisign "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/minisign")
  add_test(NAME windows_version_mapping COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_version_mapping.cmake")
endif()

if(WIN32)
  # Win32 platform layer shared by the resident, helpers and CLI.
  add_library(putgd_win_platform STATIC
    common/Paths.cpp common/Files.cpp common/Process.cpp common/Frames.cpp common/Pipe.cpp
    common/Log.cpp common/Sqlite.cpp common/Uuid.cpp)
  target_link_libraries(putgd_win_platform PUBLIC putgd_win_core shlwapi shell32 ole32 bcrypt advapi32 userenv uuid)

  add_executable(putgd WIN32
    resident/main.cpp resident/App.cpp resident/Resident.cpp resident/Tray.cpp resident/Hotkeys.cpp resident/Clipboard.cpp
    resident/Session.cpp resident/SettingsService.cpp resident/HistoryStore.cpp resident/Artifacts.cpp
    resident/OperationService.cpp resident/HistoryService.cpp resident/SetupService.cpp resident/CredentialStore.cpp
    resident/LoginStartup.cpp resident/Helpers.cpp resident/S3Provider.cpp resident/ProtonProvider.cpp resident/UpdateService.cpp resident/UpdateInstall.cpp resident/UiProcess.cpp
    s3/S3Worker.cpp s3/WinHttp.cpp
    capture/CaptureMain.cpp capture/Overlay.cpp capture/Grab.cpp capture/Png.cpp capture/ImageJob.cpp
    ui/UiMain.cpp ui/MainWindow.cpp ui/SettingsPage.cpp ui/DestinationsPage.cpp ui/HistoryPage.cpp ui/UiSession.cpp ui/Controls.cpp ui/Theme.cpp
    editor/EditorMain.cpp editor/EditDocument.cpp editor/EditorCommands.cpp editor/EditorWindow.cpp editor/Export.cpp
    resident/putgd.rc)
  target_link_libraries(putgd PRIVATE putgd_win_platform user32 gdi32 comctl32 comdlg32 uxtheme dwmapi winhttp
    windowscodecs gdiplus shcore msimg32 credui)
  target_compile_definitions(putgd PRIVATE ISOLATION_AWARE_ENABLED=1)
  # Proton Drive: the separately provisioned Windows runtime helper
  # (tools/proton/provision_runtime.py --target windows-x64). The manifest's digest
  # is compiled in; the resident runs only a helper whose bytes match it, from
  # proton\putgd-proton-runtime.exe next to putgd.exe. Without a manifest the
  # provider reports proton_runtime_unavailable.
  set(PUTGD_PROTON_MANIFEST "" CACHE FILEPATH "manifest.json of the provisioned Windows Proton runtime")
  if(PUTGD_PROTON_MANIFEST)
    file(READ "${PUTGD_PROTON_MANIFEST}" _proton_manifest)
    string(JSON _proton_sha256 GET "${_proton_manifest}" sha256)
    string(JSON _proton_platform GET "${_proton_manifest}" platform)
    string(JSON _proton_protocol GET "${_proton_manifest}" protocolVersion)
    if(NOT _proton_platform STREQUAL "windows-x64" OR NOT _proton_protocol EQUAL 1)
      message(FATAL_ERROR "The Proton runtime manifest is not a protocol-1 windows-x64 build")
    endif()
    target_compile_definitions(putgd PRIVATE "PUTGD_PROTON_RUNTIME_SHA256=\"${_proton_sha256}\"")
  endif()
  # The release signer's minisign keys (#293): update.install checks the release's
  # SHA256SUMS.minisig with its channel's key, as the Linux AppImage install does.
  foreach(_channel nightly stable)
    file(STRINGS "${PUTGD_SOURCE_ROOT}/packaging/linux/repo/keys/putgd-${_channel}.minisign.pub" _key REGEX "^RW[A-Za-z0-9+/=]+$")
    list(LENGTH _key _keys)
    if(NOT _keys EQUAL 1)
      message(FATAL_ERROR "packaging/linux/repo/keys/putgd-${_channel}.minisign.pub must hold one minisign public key")
    endif()
    string(TOUPPER "${_channel}" _upper)
    set_property(SOURCE resident/UpdateInstall.cpp APPEND PROPERTY COMPILE_DEFINITIONS "PUTGD_MINISIGN_${_upper}_KEY=\"${_key}\"")
  endforeach()
  if(MSVC)
    target_link_options(putgd PRIVATE /MANIFEST:NO)
  endif()

  add_executable(putgd-cli cli/main.cpp cli/putgd-cli.rc)
  # Resource compilers report no header dependencies: a new version must rebuild both resources.
  set_source_files_properties(resident/putgd.rc cli/putgd-cli.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/common/VersionResource.h;${CMAKE_CURRENT_SOURCE_DIR}/common/VersionInfo.rc")
  target_link_libraries(putgd-cli PRIVATE putgd_win_platform)
endif()
