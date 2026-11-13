# The log, core and credential libraries, and which package a build is for.
# The persistent log (#74): Qt Core only, so every helper supervisor can link it.
add_library(putgd_log STATIC src/core/Log.cpp src/core/RunMarker.cpp src/core/CrashRecord.cpp)
set_target_properties(putgd_log PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(putgd_log PUBLIC src)
target_link_libraries(putgd_log PUBLIC Qt6::Core putgd_build_options)
# The text-job contract (#1428): Qt Core only, so the putgd-ocr helper links it
# without putgd_core, which brings Qt GUI and Qt SQL.
add_library(putgd_text_protocol STATIC src/core/TextJobProtocol.cpp)
set_target_properties(putgd_text_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(putgd_text_protocol PUBLIC src)
target_link_libraries(putgd_text_protocol PUBLIC Qt6::Core putgd_build_options)
# The child half of that contract: the handshake and the two memfds for one job.
# putgd-ocr and the test fixture helper link it; it runs no engine.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(putgd_text_child STATIC src/ocr/TextJobChild.cpp)
  target_link_libraries(putgd_text_child PUBLIC putgd_text_protocol putgd_ui_control putgd_build_identity)
endif()
add_library(putgd_core STATIC src/core/Settings.cpp src/core/SettingsService.cpp src/core/ImagePages.cpp
  src/platform/Geometry.cpp
  src/core/ClipboardPayload.cpp src/core/ArtifactStore.cpp src/core/ImageJob.cpp src/core/PosterJob.cpp src/core/TextJob.cpp src/core/LocalArchive.cpp
  src/core/UploadNaming.cpp src/core/StorageReceipt.cpp src/core/StorageProvider.cpp
  src/core/Operation.cpp src/core/HistoryStore.cpp src/core/SanitizedPng.cpp src/core/EditorJob.cpp src/core/RecordingModel.cpp
  src/core/ReleaseLink.cpp src/core/Update.cpp src/core/SystemUpdates.cpp src/core/Distribution.cpp src/core/ZipWriter.cpp src/core/FolderZip.cpp
  src/core/HostEnvironment.cpp src/core/QrImage.cpp src/core/qrcodegen/qrcodegen.cpp)
set_target_properties(putgd_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Nayuki's QR Code generator v1.8.0 (MIT), vendored unmodified for History's
# Show QR code (#1428): the resident and putgd-ui share it through this library.
set_source_files_properties(src/core/qrcodegen/qrcodegen.cpp PROPERTIES COMPILE_OPTIONS "-w")
# Which published package this build becomes, and whether it may check for a
# newer version once a day (#293). The package jobs set the target; a build
# without one (development, source archive) never checks. A distribution
# packaging put.gd itself sets PUTGD_UPDATE_CHECK=OFF.
set(putgd_package_targets ubuntu24.04 ubuntu26.04 debian13 fedora44 archlinux appimage)
set(PUTGD_PACKAGE_TARGET "" CACHE STRING "Published package this build is for: ${putgd_package_targets}, or empty")
set_property(CACHE PUTGD_PACKAGE_TARGET PROPERTY STRINGS "" ${putgd_package_targets})
option(PUTGD_UPDATE_CHECK "Let tagged package builds check app.put.gd once a day for a newer version" ON)
# Tagged DEB/RPM builds add this signed apt/dnf repository (packaging/linux);
# the app recognises a system that updates from it. Lab builds point it elsewhere.
set(PUTGD_PACKAGE_REPOSITORY_URL "https://pkg.put.gd" CACHE STRING "Base URL of the apt/dnf repositories tagged packages add")
if(NOT PUTGD_PACKAGE_REPOSITORY_URL MATCHES "^https://[a-z0-9.-]+(/[a-z0-9._-]+)*$")
  message(FATAL_ERROR "PUTGD_PACKAGE_REPOSITORY_URL must be an https URL without a trailing slash")
endif()
if(NOT PUTGD_PACKAGE_TARGET STREQUAL "" AND NOT PUTGD_PACKAGE_TARGET IN_LIST putgd_package_targets)
  message(FATAL_ERROR "PUTGD_PACKAGE_TARGET must be one of ${putgd_package_targets}, or empty")
endif()
if(PUTGD_UPDATE_CHECK)
  set(putgd_update_check_built 1)
else()
  set(putgd_update_check_built 0)
endif()
set_source_files_properties(src/core/Distribution.cpp PROPERTIES COMPILE_DEFINITIONS
  "PUTGD_PACKAGE_TARGET_NAME=\"${PUTGD_PACKAGE_TARGET}\";PUTGD_UPDATE_CHECK_BUILT=${putgd_update_check_built};PUTGD_PACKAGE_REPOSITORY=\"${PUTGD_PACKAGE_REPOSITORY_URL}\"")
target_include_directories(putgd_core PUBLIC src)
target_link_libraries(putgd_core PUBLIC putgd_build_options putgd_build_identity putgd_log putgd_text_protocol Qt6::Core Qt6::Gui Qt6::Sql PRIVATE putgd_ui_control)
# The streaming folder zip writer (#1105) deflates through zlib directly.
find_package(ZLIB REQUIRED)
target_link_libraries(putgd_core PRIVATE ZLIB::ZLIB)

add_library(putgd_credentials STATIC src/secrets/CredentialStore.cpp)
target_link_libraries(putgd_credentials PUBLIC putgd_core PRIVATE qt6keychain)
