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
project(putgd-windows VERSION 0.1.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(PUTGD_WINDOWS_PORTABLE_TESTS "Build the portable core and its tests on this host" ${UNIX})

# Build identity: the same commit/tree/count fields as the Linux build-info.
find_package(Git QUIET)
set(PUTGD_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(_commit "unknown")
set(_count "0")
set(_dirty "unknown")
if(GIT_EXECUTABLE)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PUTGD_SOURCE_ROOT}" rev-parse HEAD OUTPUT_VARIABLE _commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PUTGD_SOURCE_ROOT}" rev-list --count HEAD OUTPUT_VARIABLE _count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PUTGD_SOURCE_ROOT}" status --porcelain OUTPUT_VARIABLE _status ERROR_QUIET)
  if(_status STREQUAL "")
    set(_dirty "false")
  else()
    set(_dirty "true")
  endif()
endif()
string(SUBSTRING "${_commit}" 0 12 _short)
set(PUTGD_VERSION "${PROJECT_VERSION}-dev.${_count}+g${_short}")
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
  core/Settings.cpp core/Naming.cpp core/Operation.cpp core/SettingsPatch.cpp core/S3Settings.cpp core/SanitizedPng.cpp
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
    resident/LoginStartup.cpp resident/Helpers.cpp resident/S3Provider.cpp resident/UpdateService.cpp resident/UiProcess.cpp
    s3/S3Worker.cpp s3/WinHttp.cpp
    capture/CaptureMain.cpp capture/Overlay.cpp capture/Grab.cpp capture/Png.cpp capture/ImageJob.cpp
    ui/UiMain.cpp ui/MainWindow.cpp ui/SettingsPage.cpp ui/DestinationsPage.cpp ui/HistoryPage.cpp ui/UiSession.cpp ui/Controls.cpp
    editor/EditorMain.cpp editor/EditDocument.cpp editor/EditorCommands.cpp editor/EditorWindow.cpp editor/Export.cpp
    resident/putgd.rc)
  target_link_libraries(putgd PRIVATE putgd_win_platform user32 gdi32 comctl32 comdlg32 uxtheme dwmapi winhttp
    windowscodecs gdiplus shcore msimg32 credui)
  target_compile_definitions(putgd PRIVATE ISOLATION_AWARE_ENABLED=1)
  if(MSVC)
    target_link_options(putgd PRIVATE /MANIFEST:NO)
  endif()

  add_executable(putgd-cli cli/main.cpp)
  target_link_libraries(putgd-cli PRIVATE putgd_win_platform)
endif()
