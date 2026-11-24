# The X11, GNOME and KDE/Wayland desktop libraries.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PUTGD_XCB REQUIRED IMPORTED_TARGET xcb xcb-randr xcb-keysyms xcb-xfixes xcb-render)
  # XInput2 raw key events let shortcuts fire while a menu holds the keyboard (#341).
  pkg_check_modules(PUTGD_XCB_XINPUT REQUIRED IMPORTED_TARGET xcb-xinput)
  # XTest sends the scroll capture's wheel steps and pointer motion on X11 (#1376).
  pkg_check_modules(PUTGD_XCB_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)
  # The screen-lock query (session and system bus) and the countdown helper
  # supervisor, shared by every desktop adapter (#1363).
  add_library(putgd_session_lock STATIC src/platform/SessionLock.cpp src/platform/CountdownProcess.cpp)
  target_include_directories(putgd_session_lock PUBLIC src)
  target_link_libraries(putgd_session_lock PUBLIC Qt6::Core putgd_build_options putgd_build_identity PRIVATE Qt6::DBus)
  add_library(putgd_x11_listeners STATIC src/platform/x11/X11Clipboard.cpp src/platform/x11/X11Shortcuts.cpp)
  target_link_libraries(putgd_x11_listeners PUBLIC putgd_app putgd_session_lock Qt6::Gui PRIVATE PkgConfig::PUTGD_XCB PkgConfig::PUTGD_XCB_XINPUT Qt6::DBus)
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
  add_library(putgd_gnome STATIC src/platform/gnome/GnomeProtocol.cpp src/platform/gnome/GnomeAdapter.cpp src/platform/gnome/GnomeExtensionSetup.cpp)
  target_link_libraries(putgd_gnome PUBLIC putgd_app putgd_session_lock Qt6::DBus)
  # The packaged Shell extension's version: a lower loaded one needs a re-login (#131).
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/integrations/gnome/metadata.json" putgd_gnome_metadata)
  string(JSON putgd_gnome_extension_version GET "${putgd_gnome_metadata}" version)
  target_compile_definitions(putgd_gnome PUBLIC PUTGD_GNOME_EXTENSION_VERSION=${putgd_gnome_extension_version})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS integrations/gnome/metadata.json)
  pkg_check_modules(PUTGD_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
  pkg_check_modules(PUTGD_XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon>=1.0.0)
  # The compositor keymap reader, shared by the KDE adapter and the shortcut recorder.
  add_library(putgd_keyboard_map STATIC src/platform/wayland/KeyboardMap.cpp)
  target_include_directories(putgd_keyboard_map PUBLIC src)
  target_link_libraries(putgd_keyboard_map PUBLIC putgd_build_options PkgConfig::PUTGD_XKBCOMMON PRIVATE PkgConfig::PUTGD_WAYLAND)
  find_program(PUTGD_WAYLAND_SCANNER wayland-scanner REQUIRED)
  set(data_control_protocol "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/wayland/protocols/ext-data-control-v1.xml")
  set(protocol_dir "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
  file(MAKE_DIRECTORY "${protocol_dir}")
  add_custom_command(OUTPUT "${protocol_dir}/ext-data-control-v1-client-protocol.h"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${data_control_protocol}" "${protocol_dir}/ext-data-control-v1-client-protocol.h"
    DEPENDS "${data_control_protocol}" VERBATIM)
  add_custom_command(OUTPUT "${protocol_dir}/ext-data-control-v1-protocol.c"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${data_control_protocol}" "${protocol_dir}/ext-data-control-v1-protocol.c"
    DEPENDS "${data_control_protocol}" VERBATIM)
  set(wlr_data_control_protocol "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/wayland/protocols/wlr-data-control-unstable-v1.xml")
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
  # Vendored, unmodified capture and toplevel protocols: ext-image-copy-capture
  # with its output and toplevel sources, and both toplevel lists.
  set(putgd_capture_protocol_sources)
  # wlr-virtual-pointer (wlr-protocols, unmodified): the scroll capture's wheel
  # steps and pointer placement on labwc and Hyprland (#1376).
  foreach(protocol_name ext-image-copy-capture-v1 ext-image-capture-source-v1 ext-foreign-toplevel-list-v1 wlr-foreign-toplevel-management-unstable-v1 wlr-virtual-pointer-unstable-v1)
    set(protocol_xml "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/wayland/protocols/${protocol_name}.xml")
    add_custom_command(OUTPUT "${protocol_dir}/${protocol_name}-client-protocol.h"
      COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${protocol_xml}" "${protocol_dir}/${protocol_name}-client-protocol.h"
      DEPENDS "${protocol_xml}" VERBATIM)
    add_custom_command(OUTPUT "${protocol_dir}/${protocol_name}-protocol.c"
      COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${protocol_xml}" "${protocol_dir}/${protocol_name}-protocol.c"
      DEPENDS "${protocol_xml}" VERBATIM)
    list(APPEND putgd_capture_protocol_sources "${protocol_dir}/${protocol_name}-client-protocol.h" "${protocol_dir}/${protocol_name}-protocol.c")
  endforeach()
  add_library(putgd_wayland_region STATIC src/platform/wayland/RegionProcess.cpp src/platform/wayland/RegionProtocol.cpp)
  target_link_libraries(putgd_wayland_region PUBLIC putgd_app putgd_capture_control putgd_build_identity putgd_visual)
  find_package(Threads REQUIRED)
  # Compositor-neutral Wayland code: data control, outputs, the layer-shell
  # region selector and the fd reader. Desktop adapters link it.
  add_library(putgd_wayland STATIC src/platform/wayland/AsyncFdReader.cpp
    src/platform/wayland/ClipboardKeeper.cpp src/platform/wayland/DataControlClipboard.cpp src/platform/wayland/DataControlSourceAdapter.cpp
    src/platform/wayland/LayerShellRegionOverlay.cpp src/platform/wayland/WaylandOutputs.cpp
    src/platform/wayland/ImageCopyCapture.cpp src/platform/wayland/CaptureConnection.cpp src/platform/wayland/CursorOutputProbe.cpp src/platform/wayland/PortalGlobalShortcuts.cpp src/platform/wayland/ShortcutBackend.h src/platform/wayland/WaylandTransform.h
    ${putgd_capture_protocol_sources}
    "${protocol_dir}/ext-data-control-v1-client-protocol.h" "${protocol_dir}/ext-data-control-v1-protocol.c"
    "${protocol_dir}/wlr-data-control-unstable-v1-client-protocol.h" "${protocol_dir}/wlr-data-control-unstable-v1-protocol.c"
    "${protocol_dir}/xdg-output-unstable-v1-client-protocol.h" "${protocol_dir}/xdg-output-unstable-v1-protocol.c")
  target_include_directories(putgd_wayland PRIVATE "${protocol_dir}")
  target_link_libraries(putgd_wayland PUBLIC putgd_app putgd_wayland_region putgd_session_lock putgd_keyboard_map Qt6::Widgets Qt6::DBus PkgConfig::PUTGD_XKBCOMMON PRIVATE PkgConfig::PUTGD_WAYLAND Threads::Threads)
  # The capture sequence Budgie and Hyprland share (#1446).
  target_sources(putgd_wayland PRIVATE src/platform/wayland/WaylandCaptureAdapter.cpp)
  add_library(putgd_kde STATIC src/platform/kde/KWinTransfer.cpp src/platform/kde/KWinCapture.cpp src/platform/kde/KWinClickEffect.cpp
    src/platform/kde/KdeShortcuts.cpp src/platform/kde/KdeShortcutNormalization.cpp
    src/platform/kde/KdeSourceAdapter.cpp src/platform/kde/KWinWindows.cpp)
  target_include_directories(putgd_kde PRIVATE "${protocol_dir}")
  target_link_libraries(putgd_kde PUBLIC putgd_wayland PRIVATE PkgConfig::PUTGD_WAYLAND Threads::Threads)
  # Budgie 10.10 (labwc) and Magpie: capture, toplevels and data control from
  # the shared Wayland code; the labwc bridge and D-Bus trigger are Budgie's.
  add_library(putgd_budgie STATIC src/platform/budgie/BudgieSourceAdapter.cpp src/platform/budgie/BudgieShortcuts.cpp
    src/platform/budgie/LabwcBridgeBackend.cpp src/platform/budgie/BudgieClicks.cpp)
  target_link_libraries(putgd_budgie PUBLIC putgd_wayland Qt6::DBus PRIVATE PkgConfig::PUTGD_XKBCOMMON)
  # Hyprland (Omarchy and other Hyprland sessions): the shared capture, outputs,
  # data control and region helper; Hyprland's IPC, toplevel mapping check and
  # global-shortcut protocol are its own. The two hyprland-protocols XML files
  # are vendored unmodified (hyprwm/hyprland-protocols cd1aba6, BSD-3-Clause),
  # and wlr-layer-shell likewise (wlr-protocols b010a03).
  set(putgd_hyprland_protocol_sources)
  foreach(protocol_name hyprland-global-shortcuts-v1 hyprland-toplevel-mapping-v1 wlr-layer-shell-unstable-v1)
    set(protocol_xml "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/wayland/protocols/${protocol_name}.xml")
    add_custom_command(OUTPUT "${protocol_dir}/${protocol_name}-client-protocol.h"
      COMMAND "${PUTGD_WAYLAND_SCANNER}" client-header "${protocol_xml}" "${protocol_dir}/${protocol_name}-client-protocol.h"
      DEPENDS "${protocol_xml}" VERBATIM)
    add_custom_command(OUTPUT "${protocol_dir}/${protocol_name}-protocol.c"
      COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${protocol_xml}" "${protocol_dir}/${protocol_name}-protocol.c"
      DEPENDS "${protocol_xml}" VERBATIM)
    list(APPEND putgd_hyprland_protocol_sources "${protocol_dir}/${protocol_name}-client-protocol.h" "${protocol_dir}/${protocol_name}-protocol.c")
  endforeach()
  # wlr-layer-shell's get_popup names xdg_popup, so its code needs xdg-shell's interfaces.
  set(xdg_shell_protocol "${PUTGD_WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
  add_custom_command(OUTPUT "${protocol_dir}/xdg-shell-protocol.c"
    COMMAND "${PUTGD_WAYLAND_SCANNER}" private-code "${xdg_shell_protocol}" "${protocol_dir}/xdg-shell-protocol.c"
    DEPENDS "${xdg_shell_protocol}" VERBATIM)
  # Shared visibility probes and Hyprland's catcher use the same layer protocol.
  list(FILTER putgd_hyprland_protocol_sources EXCLUDE REGEX "wlr-layer-shell")
  target_sources(putgd_wayland PRIVATE src/platform/wayland/SurfaceVisibility.cpp
    "${protocol_dir}/wlr-layer-shell-unstable-v1-client-protocol.h"
    "${protocol_dir}/wlr-layer-shell-unstable-v1-protocol.c" "${protocol_dir}/xdg-shell-protocol.c")
  add_library(putgd_hyprland STATIC src/platform/hyprland/HyprlandIpc.cpp src/platform/hyprland/HyprlandClicks.cpp src/platform/hyprland/HyprlandShortcuts.cpp
    src/platform/hyprland/HyprlandSourceAdapter.cpp src/platform/hyprland/HyprlandWayland.cpp src/platform/hyprland/PressCatcher.cpp src/platform/hyprland/ToplevelMapping.cpp
    ${putgd_hyprland_protocol_sources} "${protocol_dir}/ext-foreign-toplevel-list-v1-client-protocol.h")
  target_include_directories(putgd_hyprland PRIVATE "${protocol_dir}")
  target_link_libraries(putgd_hyprland PUBLIC putgd_wayland PRIVATE PkgConfig::PUTGD_WAYLAND PkgConfig::PUTGD_XKBCOMMON Threads::Threads)
  if(Qt6_VERSION VERSION_GREATER_EQUAL "6.5")
    find_package(LayerShellQt QUIET CONFIG)
    if(TARGET LayerShellQt::Interface)
      foreach(layer_shell_user putgd_wayland putgd_kde)
        target_link_libraries(${layer_shell_user} PRIVATE LayerShellQt::Interface)
        target_compile_definitions(${layer_shell_user} PRIVATE PUTGD_HAVE_LAYER_SHELL=1)
      endforeach()
      # Budgie and Hyprland select regions only through the putgd-region helper.
      target_compile_definitions(putgd_budgie PRIVATE PUTGD_HAVE_LAYER_SHELL=1)
      target_compile_definitions(putgd_hyprland PRIVATE PUTGD_HAVE_LAYER_SHELL=1)
      set(PUTGD_LAYER_SHELL_REGION ON)
    else()
      message(STATUS "LayerShellQt unavailable: Wayland layer-shell region selection will report unavailable")
    endif()
  endif()
endif()
