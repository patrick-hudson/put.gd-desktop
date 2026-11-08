# The X11, GNOME and KDE/Wayland desktop libraries.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PUTGD_XCB REQUIRED IMPORTED_TARGET xcb xcb-randr xcb-keysyms xcb-xfixes xcb-render)
  # XInput2 raw key events let shortcuts fire while a menu holds the keyboard (#341).
  pkg_check_modules(PUTGD_XCB_XINPUT REQUIRED IMPORTED_TARGET xcb-xinput)
  add_library(putgd_x11_listeners STATIC src/platform/x11/X11Clipboard.cpp src/platform/x11/X11Shortcuts.cpp src/platform/x11/X11SessionLock.cpp)
  target_link_libraries(putgd_x11_listeners PUBLIC putgd_app Qt6::Gui PRIVATE PkgConfig::PUTGD_XCB PkgConfig::PUTGD_XCB_XINPUT Qt6::DBus)
  pkg_check_modules(PUTGD_XCB_DAMAGE REQUIRED IMPORTED_TARGET xcb-damage)
  add_library(putgd_x11 STATIC src/platform/x11/X11Capture.cpp src/platform/x11/CinnamonRegionCapture.cpp
    src/platform/x11/X11CompositorSettle.cpp src/platform/x11/X11SourceAdapter.cpp src/platform/x11/NativeRegionOverlay.cpp
    src/ui/RegionOverlay.cpp src/ui/RegionSelector.h)
  target_link_libraries(putgd_x11 PUBLIC putgd_app putgd_x11_listeners putgd_visual Qt6::Widgets PRIVATE PkgConfig::PUTGD_XCB PkgConfig::PUTGD_XCB_DAMAGE Qt6::DBus)
  add_library(putgd_capture_control STATIC src/ipc/CaptureControl.cpp)
  target_include_directories(putgd_capture_control PUBLIC src)
  target_link_libraries(putgd_capture_control PUBLIC Qt6::Core putgd_build_options)
  add_library(putgd_x11_native_snapshot STATIC src/platform/x11/X11NativeSnapshot.cpp)
  target_include_directories(putgd_x11_native_snapshot PUBLIC src)
  target_link_libraries(putgd_x11_native_snapshot PUBLIC Qt6::Core putgd_build_options PRIVATE PkgConfig::PUTGD_XCB)
  add_library(putgd_capture_cursor STATIC src/platform/x11/CaptureCursorCache.cpp)
  target_include_directories(putgd_capture_cursor PUBLIC src)
  target_link_libraries(putgd_capture_cursor PUBLIC Qt6::Core putgd_build_options PRIVATE PkgConfig::PUTGD_XCB)
  add_library(putgd_x11_core STATIC src/platform/x11/X11CoreSourceAdapter.cpp src/platform/x11/CaptureProcessController.cpp)
  target_link_libraries(putgd_x11_core PUBLIC putgd_app putgd_x11_listeners putgd_x11_native_snapshot putgd_capture_control putgd_capture_cursor putgd_build_identity)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(putgd_gnome STATIC src/platform/gnome/GnomeProtocol.cpp src/platform/gnome/GnomeAdapter.cpp)
  target_link_libraries(putgd_gnome PUBLIC putgd_app Qt6::DBus)
  # The packaged Shell extension's version: a lower loaded one needs a re-login (#131).
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/integrations/gnome/metadata.json" putgd_gnome_metadata)
  string(JSON putgd_gnome_extension_version GET "${putgd_gnome_metadata}" version)
  target_compile_definitions(putgd_gnome PUBLIC PUTGD_GNOME_EXTENSION_VERSION=${putgd_gnome_extension_version})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS integrations/gnome/metadata.json)
  pkg_check_modules(PUTGD_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
  pkg_check_modules(PUTGD_XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon>=1.0.0)
  # The compositor keymap reader, shared by the KDE adapter and the shortcut recorder.
  add_library(putgd_keyboard_map STATIC src/platform/kde/KdeKeyboardMap.cpp)
  target_include_directories(putgd_keyboard_map PUBLIC src)
  target_link_libraries(putgd_keyboard_map PUBLIC putgd_build_options PkgConfig::PUTGD_XKBCOMMON PRIVATE PkgConfig::PUTGD_WAYLAND)
  find_program(PUTGD_WAYLAND_SCANNER wayland-scanner REQUIRED)
  set(data_control_protocol "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/kde/protocols/ext-data-control-v1.xml")
  set(protocol_dir "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
  file(MAKE_DIRECTORY "${protocol_dir}")
  add_custom_command(OUTPUT "${protocol_dir}/ext-data-control-v1-client-protocol.h"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${data_control_protocol}" "${protocol_dir}/ext-data-control-v1-client-protocol.h"
    DEPENDS "${data_control_protocol}" VERBATIM)
  add_custom_command(OUTPUT "${protocol_dir}/ext-data-control-v1-protocol.c"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${data_control_protocol}" "${protocol_dir}/ext-data-control-v1-protocol.c"
    DEPENDS "${data_control_protocol}" VERBATIM)
  set(wlr_data_control_protocol "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/kde/protocols/wlr-data-control-unstable-v1.xml")
  add_custom_command(OUTPUT "${protocol_dir}/wlr-data-control-unstable-v1-client-protocol.h"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${wlr_data_control_protocol}" "${protocol_dir}/wlr-data-control-unstable-v1-client-protocol.h"
    DEPENDS "${wlr_data_control_protocol}" VERBATIM)
  add_custom_command(OUTPUT "${protocol_dir}/wlr-data-control-unstable-v1-protocol.c"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${wlr_data_control_protocol}" "${protocol_dir}/wlr-data-control-unstable-v1-protocol.c"
    DEPENDS "${wlr_data_control_protocol}" VERBATIM)
  pkg_get_variable(PUTGD_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  if(NOT PUTGD_WAYLAND_PROTOCOLS_DIR)
    message(FATAL_ERROR "The installed wayland-protocols development package is required")
  endif()
  set(xdg_output_protocol "${PUTGD_WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml")
  add_custom_command(OUTPUT "${protocol_dir}/xdg-output-unstable-v1-client-protocol.h"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${xdg_output_protocol}" "${protocol_dir}/xdg-output-unstable-v1-client-protocol.h"
    DEPENDS "${xdg_output_protocol}" VERBATIM)
  add_custom_command(OUTPUT "${protocol_dir}/xdg-output-unstable-v1-protocol.c"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${xdg_output_protocol}" "${protocol_dir}/xdg-output-unstable-v1-protocol.c"
    DEPENDS "${xdg_output_protocol}" VERBATIM)
  add_library(putgd_kde_region_process STATIC src/platform/kde/KdeRegionProcess.cpp src/platform/kde/KdeRegionProtocol.cpp)
  target_link_libraries(putgd_kde_region_process PUBLIC putgd_app putgd_capture_control putgd_build_identity putgd_visual)
  find_package(Threads REQUIRED)
  add_library(putgd_kde STATIC src/platform/kde/KWinTransfer.cpp src/platform/kde/KWinCapture.cpp
    src/platform/kde/KdeShortcuts.cpp src/platform/kde/KdeShortcutNormalization.cpp
    src/platform/wayland/DataControlClipboard.cpp src/platform/wayland/DataControlSourceAdapter.cpp
    src/platform/kde/KdeRegionOverlay.cpp src/platform/kde/KdeSourceAdapter.cpp src/platform/kde/KdeOutputs.cpp src/platform/kde/KWinWindows.cpp
    "${protocol_dir}/ext-data-control-v1-client-protocol.h" "${protocol_dir}/ext-data-control-v1-protocol.c"
    "${protocol_dir}/wlr-data-control-unstable-v1-client-protocol.h" "${protocol_dir}/wlr-data-control-unstable-v1-protocol.c"
    "${protocol_dir}/xdg-output-unstable-v1-client-protocol.h" "${protocol_dir}/xdg-output-unstable-v1-protocol.c")
  target_include_directories(putgd_kde PRIVATE "${protocol_dir}")
  target_link_libraries(putgd_kde PUBLIC putgd_app putgd_kde_region_process putgd_keyboard_map Qt6::Widgets Qt6::DBus PkgConfig::PUTGD_XKBCOMMON PRIVATE PkgConfig::PUTGD_WAYLAND Threads::Threads)
  if(Qt6_VERSION VERSION_GREATER_EQUAL "6.5")
    find_package(LayerShellQt QUIET CONFIG)
    if(TARGET LayerShellQt::Interface)
      target_link_libraries(putgd_kde PRIVATE LayerShellQt::Interface)
      target_compile_definitions(putgd_kde PRIVATE PUTGD_HAVE_LAYER_SHELL=1)
      set(PUTGD_KDE_NATIVE_REGION ON)
    else()
      message(STATUS "LayerShellQt unavailable: KDE native region selection will report unavailable")
    endif()
  endif()
endif()
