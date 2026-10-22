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
#   putgd.exe --record        screen recording: GDI capture, Media Foundation H.264 MP4
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

add_library(putgd_win_options INTERFACE)
target_include_directories(putgd_win_options INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(MSVC)
  target_compile_options(putgd_win_options INTERFACE /W4 /utf-8 /EHsc /permissive-)
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
  core/Settings.cpp core/Naming.cpp core/Operation.cpp core/SettingsPatch.cpp core/S3Settings.cpp core/SanitizedPng.cpp core/ProtonSettings.cpp core/Recording.cpp core/RecordingFrames.cpp core/Mp4.cpp core/BarPlacement.cpp core/ReviewTimeline.cpp core/Audio.cpp
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
  add_executable(test_win_recording tests/test_recording.cpp)
  target_link_libraries(test_win_recording PRIVATE putgd_win_core)
  add_test(NAME windows_recording_contract COMMAND test_win_recording)
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
    resident/LoginStartup.cpp resident/Helpers.cpp resident/Recording.cpp resident/S3Provider.cpp resident/ProtonProvider.cpp resident/UpdateService.cpp resident/UiProcess.cpp
    s3/S3Worker.cpp s3/WinHttp.cpp
    capture/CaptureMain.cpp capture/Overlay.cpp capture/Grab.cpp capture/Png.cpp capture/ImageJob.cpp
    ui/UiMain.cpp ui/MainWindow.cpp ui/SettingsPage.cpp ui/DestinationsPage.cpp ui/HistoryPage.cpp ui/UiSession.cpp ui/Controls.cpp ui/Theme.cpp
    editor/EditorMain.cpp editor/EditDocument.cpp editor/EditorCommands.cpp editor/EditorWindow.cpp editor/Export.cpp
    record/RecordMain.cpp record/Recorder.cpp record/Sources.cpp record/Media.cpp record/Export.cpp record/Overlay.cpp record/FrameSource.cpp record/ReviewWindow.cpp record/Gif.cpp
    resident/putgd.rc)
  target_link_libraries(putgd PRIVATE putgd_win_platform user32 gdi32 comctl32 comdlg32 uxtheme dwmapi winhttp
    windowscodecs gdiplus shcore msimg32 credui
    # GUIDs only: Media Foundation itself is loaded at run time by putgd.exe --record (record/Media.cpp).
    mfuuid)
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
  if(MSVC)
    target_link_options(putgd PRIVATE /MANIFEST:NO)
  endif()

  add_executable(putgd-cli cli/main.cpp)
  target_link_libraries(putgd-cli PRIVATE putgd_win_platform)
endif()
