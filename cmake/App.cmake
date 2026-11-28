# The application library and the files it compiles in.
add_library(putgd_app STATIC src/app/OperationService.cpp src/app/ErrorLog.cpp src/app/DestinationStatus.cpp src/app/OperationClipboard.cpp src/app/UploadSlots.cpp src/app/ScrollCaptureService.cpp src/app/RecordingService.cpp src/app/RecordingHelperBackend.cpp src/app/HistoryService.cpp src/app/SetupService.cpp src/app/DestinationTestJournal.cpp src/app/LocalServer.cpp src/app/LocalIpc.cpp src/app/ResidentSession.cpp src/app/LoginStartup.cpp src/app/StorageLocations.cpp src/app/LogService.cpp src/app/DiagnosticsBundle.cpp src/app/UpdateService.cpp src/app/AppImageInstall.cpp src/app/FileUploads.cpp src/app/FileManagerIntegration.cpp src/app/EncryptCheck.cpp src/app/FalseAlarmReports.cpp)
target_sources(putgd_app PRIVATE src/app/ScrollHelperBackend.cpp src/app/HelperSupervisor.cpp)
# Text recognition (#1428): the resident's interface to the putgd-ocr helper.
target_sources(putgd_app PRIVATE src/app/TextService.cpp)
target_link_libraries(putgd_app PUBLIC putgd_core PRIVATE putgd_credentials putgd_ui_control putgd_quick_redact putgd_encrypt_page ${CMAKE_DL_LIBS})
# The AppImage login script LoginStartup writes (#307), compiled in from its source.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/app/appimage-login-start.sh" putgd_login_start_script)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/app/LoginStartScript.h"
  CONTENT "#pragma once\nnamespace putgd {\ninline constexpr char putgdLoginStartScript[]=R\"putgdsh(${putgd_login_start_script})putgdsh\";\n}\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/app/appimage-login-start.sh)
# The file-manager integration's scripts and the packaged menu files it writes
# per user for the AppImage (#1105), compiled in from their sources.
set(putgd_file_manager_templates "#pragma once\nnamespace putgd {\n")
foreach(template IN ITEMS FileManagerScript=src/app/file-manager-script.sh FileManagerLauncher=src/app/file-manager-launcher.sh
    KioServiceMenu=packaging/linux/filemanager/kio-servicemenu.desktop NemoAction=packaging/linux/filemanager/putgd-upload.nemo_action
    ThunarSendTo=packaging/linux/filemanager/thunar-sendto.desktop FileManagerAction=packaging/linux/filemanager/file-manager-action.desktop
    Contract=packaging/linux/filemanager/putgd-upload.contract
    NemoActionEncrypted=packaging/linux/filemanager/putgd-upload-encrypted.nemo_action
    ThunarSendToEncrypted=packaging/linux/filemanager/thunar-sendto-encrypted.desktop
    FileManagerActionEncrypted=packaging/linux/filemanager/file-manager-action-encrypted.desktop
    ContractEncrypted=packaging/linux/filemanager/putgd-upload-encrypted.contract
    # Earlier templates older AppImages wrote, so the package recognizes exactly those copies (#2119).
    KioServiceMenu1105=src/app/file-manager-legacy/kio-servicemenu-1105.desktop
    FileManagerScript987483d=src/app/file-manager-legacy/file-manager-script-987483d.sh
    FileManagerScriptFb3a313=src/app/file-manager-legacy/file-manager-script-fb3a313.sh
    # The encrypted Nemo and FileManager-Actions entries before their visibility test (#2136).
    NemoActionEncrypted4631e27=src/app/file-manager-legacy/putgd-upload-encrypted-4631e27.nemo_action
    FileManagerActionEncrypted4631e27=src/app/file-manager-legacy/file-manager-action-encrypted-4631e27.desktop
    # The AppImage's stable launcher as each earlier build wrote it, before entries carried hashes (#2132).
    FileManagerLauncher987483d=src/app/file-manager-legacy/file-manager-launcher-987483d.sh
    FileManagerLauncher2f1bc55=src/app/file-manager-legacy/file-manager-launcher-2f1bc55.sh
    FileManagerLauncher217621f=src/app/file-manager-legacy/file-manager-launcher-217621f.sh
    FileManagerLauncher4631e27=src/app/file-manager-legacy/file-manager-launcher-4631e27.sh)
  string(REPLACE "=" ";" template "${template}")
  list(GET template 0 template_name)
  list(GET template 1 template_path)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${template_path}" template_text)
  string(APPEND putgd_file_manager_templates "inline constexpr char putgd${template_name}[]=R\"putgdsh(${template_text})putgdsh\";\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${template_path}")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/app/FileManagerTemplates.h" CONTENT "${putgd_file_manager_templates}}\n" @ONLY)
target_include_directories(putgd_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/app")
