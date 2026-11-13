# The interface libraries, down to putgd_ui_application.
add_library(putgd_ui_control STATIC src/ipc/UiControl.cpp src/ipc/StdioDevice.cpp)
target_include_directories(putgd_ui_control PUBLIC src)
target_link_libraries(putgd_ui_control PUBLIC Qt6::Core putgd_build_options)
add_library(putgd_ui_supervisor STATIC src/app/UiProcessController.cpp)
target_link_libraries(putgd_ui_supervisor PUBLIC putgd_ui_control putgd_build_identity putgd_log)
# The session.sock request/reply envelope and deadlines, Core-only, so the
# resident's own --upload client never brings QtNetwork into the resident.
add_library(putgd_session_protocol STATIC src/client/SessionProtocol.cpp)
target_include_directories(putgd_session_protocol PUBLIC src)
target_link_libraries(putgd_session_protocol PUBLIC Qt6::Core putgd_build_options)
add_library(putgd_client STATIC src/client/SessionClient.cpp)
target_include_directories(putgd_client PUBLIC src)
target_link_libraries(putgd_client PUBLIC putgd_session_protocol Qt6::Core putgd_build_options)
# Linux clients speak to the resident over a native Unix socket, so putgd-ui
# maps no QtNetwork (nor its libproxy/curl/TLS chain, #1161); other platforms
# use QLocalSocket.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(putgd_client PUBLIC Qt6::Network)
endif()
add_library(putgd_ui_state STATIC src/ui/ViewState.cpp)
target_include_directories(putgd_ui_state PUBLIC src)
target_link_libraries(putgd_ui_state PUBLIC Qt6::Core putgd_build_options)
add_library(putgd_visual STATIC src/ui/Ui.cpp src/ui/IconArt.cpp src/ui/Theme.cpp src/ui/WindowButtons.cpp)
target_include_directories(putgd_visual PUBLIC src)
target_link_libraries(putgd_visual PUBLIC Qt6::Widgets putgd_build_options)
# Links and folders opened on the host (#326): the desktop portal and the file
# manager over D-Bus, else a launcher with the host's environment. The resident
# and putgd-ui share it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(putgd_host_open STATIC src/platform/HostOpen.cpp)
  target_link_libraries(putgd_host_open PUBLIC putgd_core Qt6::DBus)
else()
  add_library(putgd_host_open STATIC src/platform/HostOpenDesktopServices.cpp)
  target_link_libraries(putgd_host_open PUBLIC putgd_core)
endif()
add_library(putgd_tray STATIC src/ui/TrayController.cpp src/ui/PanelTone.cpp src/ui/TrayCalendar.cpp)
target_link_libraries(putgd_tray PUBLIC putgd_app putgd_visual putgd_host_open)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(putgd_tray PRIVATE src/ui/NativeTray.cpp)
  target_link_libraries(putgd_tray PUBLIC Qt6::DBus)
endif()
add_library(putgd_about STATIC src/ui/AboutWindow.cpp)
target_link_libraries(putgd_about PUBLIC putgd_visual putgd_build_identity PRIVATE putgd_host_open putgd_fonts)
# The appearance setting resolved against the desktop's preference (#60). The
# interface and the resident (for putgd-record's overlays, #198) share it.
add_library(putgd_appearance STATIC src/ui/Appearance.cpp)
target_link_libraries(putgd_appearance PUBLIC putgd_visual)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The desktop's colour-scheme preference through the XDG portal (#60).
  target_sources(putgd_appearance PRIVATE src/ui/PortalSettings.cpp)
  target_link_libraries(putgd_appearance PUBLIC Qt6::DBus)
endif()
# Open, save and folder dialogs (#259): the desktop's chooser through the XDG
# portal, or Qt's widget dialog in the Theme tokens. putgd-ui and putgd-record
# share it; neither loads a desktop platform theme for it.
add_library(putgd_file_dialog STATIC src/ui/FileDialog.cpp)
target_link_libraries(putgd_file_dialog PUBLIC putgd_visual)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(putgd_file_dialog PUBLIC Qt6::DBus)
endif()
# The shortcut recorder's view of the session keyboard map (Wayland or X11).
add_library(putgd_shortcut_keyboard STATIC src/ui/ShortcutKeyboard.cpp)
target_include_directories(putgd_shortcut_keyboard PUBLIC src)
target_link_libraries(putgd_shortcut_keyboard PUBLIC Qt6::Gui putgd_build_options)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(putgd_shortcut_keyboard PRIVATE PUTGD_SHORTCUT_KEYBOARD=1)
  target_link_libraries(putgd_shortcut_keyboard PRIVATE putgd_keyboard_map PkgConfig::PUTGD_XKBCOMMON PkgConfig::PUTGD_WAYLAND PkgConfig::PUTGD_XCB)
endif()
# The bundled IBM Plex fonts (OFL), for the interface and recording helper only.
add_library(putgd_fonts STATIC src/ui/Fonts.cpp src/ui/fonts.qrc)
target_link_libraries(putgd_fonts PUBLIC putgd_visual)
# Fonts.cpp looks for the installed copies in the configured data directory:
# beside bin/ when it is relative (relocatable, as the AppImage is).
# The data directory relative to the bin directory, so the lookup also fits
# layouts where both sit under a "/" prefix as usr/bin and usr/share.
if(IS_ABSOLUTE "${CMAKE_INSTALL_DATADIR}")
  set(PUTGD_FONTS_DATADIR "${CMAKE_INSTALL_DATADIR}")
else()
  file(RELATIVE_PATH PUTGD_FONTS_DATADIR "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_DATADIR}")
endif()
target_compile_definitions(putgd_fonts PRIVATE PUTGD_INSTALL_DATADIR="${PUTGD_FONTS_DATADIR}")
# The same font files installed beside the executables: Fonts.cpp maps these
# instead of copying the resource fonts into every interface process's heap.
install(FILES src/ui/fonts/IBMPlexSans-Regular.ttf src/ui/fonts/IBMPlexSans-Medium.ttf src/ui/fonts/IBMPlexSans-SemiBold.ttf
  src/ui/fonts/IBMPlexMono-Regular.ttf DESTINATION "${CMAKE_INSTALL_DATADIR}/putgd/fonts")
# No probe OpenGL context from Plasma's platform theme in processes that draw
# no Qt Quick (#1161).
add_library(putgd_quick_backend STATIC src/ui/QuickBackend.cpp)
target_include_directories(putgd_quick_backend PUBLIC src)
target_link_libraries(putgd_quick_backend PUBLIC Qt6::Core putgd_build_options PRIVATE ${CMAKE_DL_LIBS})
# Modal dialogs that move on their own on GNOME instead of dragging their
# parent window along.
add_library(putgd_dialog_attachment STATIC src/ui/DialogAttachment.cpp)
target_include_directories(putgd_dialog_attachment PUBLIC src)
target_link_libraries(putgd_dialog_attachment PUBLIC Qt6::Core putgd_build_options PRIVATE putgd_core)
# PNG previews decode through the built-in handler by name (#1161).
add_library(putgd_preview_reader STATIC src/ui/PreviewReader.cpp)
target_include_directories(putgd_preview_reader PUBLIC src)
target_link_libraries(putgd_preview_reader PUBLIC Qt6::Gui putgd_build_options)
add_library(putgd_ui STATIC src/ui/MainWindow.cpp src/ui/UiWindow.cpp src/ui/WindowActivation.cpp src/ui/UiSession.cpp src/ui/SettingsWindow.cpp src/ui/CustomUploaderPanel.cpp src/ui/ShortcutEditor.cpp
  src/ui/HistoryWindow.cpp src/ui/HistoryModel.cpp src/ui/QrCodeWindow.cpp)
target_link_libraries(putgd_ui PUBLIC putgd_core putgd_client putgd_ui_state putgd_visual putgd_fonts putgd_appearance putgd_file_dialog PRIVATE putgd_s3_wire putgd_host_open putgd_shortcut_keyboard putgd_preview_reader)
# Editing model and canvas for the on-demand editor helper; no storage or IPC.
add_library(putgd_editor STATIC src/editor/EditDocument.cpp src/editor/EditorCommands.cpp src/editor/EditorCanvas.cpp src/editor/HighlightFilter.cpp src/editor/TextLayer.cpp src/editor/QuickRedact.cpp)
target_include_directories(putgd_editor PUBLIC src)
target_link_libraries(putgd_editor PUBLIC Qt6::Widgets putgd_build_options putgd_visual)
add_library(putgd_editor_window STATIC src/ui/EditorWindow.cpp src/ui/EditorText.cpp)
target_link_libraries(putgd_editor_window PUBLIC putgd_editor putgd_ui putgd_visual PRIVATE putgd_host_open)
add_library(putgd_ui_application STATIC src/ui/UiApplicationController.cpp)
target_link_libraries(putgd_ui_application PUBLIC putgd_ui putgd_about putgd_ui_control)
