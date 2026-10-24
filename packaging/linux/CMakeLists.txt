# Root integrates this directory after defining all three product executables.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
include("${PROJECT_SOURCE_DIR}/cmake/InstallPackageSnapshot.cmake")
putgd_install_snapshot(putgd-ui.desktop "${CMAKE_INSTALL_DATADIR}/applications")
putgd_install_snapshot(putgd.svg "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
option(PUTGD_LAB_FIXTURE "Package independent synthetic desktop fixture (development only)" OFF)
configure_file(app.desktop.in putgd.desktop @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/putgd.desktop"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
if(TARGET putgd-record)
  # KWin grants its screencast protocol to the executable named by this entry's Exec.
  configure_file(record.desktop.in putgd-record.desktop @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/putgd-record.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
endif()
foreach(gnome_asset extension.js metadata.json)
  putgd_install_snapshot("${PROJECT_SOURCE_DIR}/integrations/gnome/${gnome_asset}"
    "${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/putgd@put.gd")
endforeach()
# This is an opt-in XDG autostart template; preferences.launchAtLogin owns enabling it.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/putgd.desktop"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/putgd/autostart")
if(PUTGD_LAB_FIXTURE)
  add_executable(putgd-desktop-fixture "${PROJECT_SOURCE_DIR}/tests/fixtures/desktop_fixture.cpp")
  target_link_libraries(putgd-desktop-fixture PRIVATE putgd_build_options Qt6::Widgets)
  install(TARGETS putgd-desktop-fixture RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# SDK provisioned by docs/build.md remains privately bundled and replaceable; Qt is distro-owned.
set(PUTGD_AWS_RUNTIME_DIR "${PROJECT_SOURCE_DIR}/build/sdk/lib" CACHE PATH "Official SDK runtime library directory")
file(GLOB aws_runtime "${PUTGD_AWS_RUNTIME_DIR}/libaws*.so*" "${PUTGD_AWS_RUNTIME_DIR}/libs2n.so*")
if(NOT aws_runtime)
  message(FATAL_ERROR "Official AWS shared runtime missing: provision docs/build.md or set PUTGD_AWS_RUNTIME_DIR")
endif()
install(FILES ${aws_runtime} DESTINATION "${CMAKE_INSTALL_LIBDIR}/putgd")
set(runtime_manifest "{\n  \"schemaVersion\": 1,\n  \"libraries\": [\n")
set(runtime_separator "")
foreach(runtime_file IN LISTS aws_runtime)
  get_filename_component(runtime_name "${runtime_file}" NAME)
  file(SHA256 "${runtime_file}" runtime_hash)
  string(APPEND runtime_manifest "${runtime_separator}    {\"file\": \"${runtime_name}\", \"sha256\": \"${runtime_hash}\"}")
  set(runtime_separator ",\n")
endforeach()
string(APPEND runtime_manifest "\n  ]\n}\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/aws-runtime-manifest.json" "${runtime_manifest}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/aws-runtime-manifest.json" DESTINATION "${CMAKE_INSTALL_DATADIR}/putgd")
set(PUTGD_S3_MODULE_FILENAME "$<TARGET_FILE_NAME:putgd_s3_module>")
set(s3_manifest_install_code [=[
  set(module_name "@PUTGD_S3_MODULE_FILENAME@")
  set(module_path "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/@CMAKE_INSTALL_LIBDIR@/putgd/${module_name}")
  file(SHA256 "${module_path}" module_hash)
  set(manifest_path "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/@CMAKE_INSTALL_DATADIR@/putgd/aws-runtime-manifest.json")
  file(READ "${manifest_path}" manifest)
  string(JSON manifest SET "${manifest}" module "{\"file\":\"${module_name}\",\"contractVersion\":1,\"buildIdentity\":\"@PUTGD_S3_BUILD_ID@\",\"sha256\":\"${module_hash}\"}")
  file(WRITE "${manifest_path}" "${manifest}\n")
]=])
string(CONFIGURE "${s3_manifest_install_code}" s3_manifest_install_code @ONLY)
install(CODE "${s3_manifest_install_code}")
target_link_options(putgd_s3_module PRIVATE "LINKER:--disable-new-dtags")
set_target_properties(putgd_s3_module PROPERTIES BUILD_RPATH "${PUTGD_AWS_RUNTIME_DIR}" INSTALL_RPATH "$ORIGIN")
# Every SDK shared object also resolves its CRT siblings from this private directory.
# The S3 module uses DT_RPATH so SDK dependencies resolve transitively; the launcher
# also scopes the private directory to this process for explicit desktop startup.
configure_file(putgd-launcher.in putgd-launcher @ONLY)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/putgd-launcher" DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME putgd-launcher)
putgd_install_snapshot("${PROJECT_SOURCE_DIR}/cmake/dependency-lock.json" "${CMAKE_INSTALL_DATADIR}/putgd")
# The notices name the release page that carries this build's source (#416, #469).
set(PUTGD_SOURCE_URL "https://github.com/patrick-hudson/put.gd-desktop/releases")
if(PUTGD_RELEASE_TAG)
  string(APPEND PUTGD_SOURCE_URL "/tag/${PUTGD_RELEASE_TAG}")
endif()
configure_file(runtime-notices.txt.in package-inputs/runtime-notices.txt @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/package-inputs/runtime-notices.txt" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/putgd")
putgd_install_snapshot("${PROJECT_SOURCE_DIR}/src/platform/kde/protocols/ext-data-control-v1.xml"
  "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME wayland-ext-data-control-v1.xml)
putgd_install_snapshot("${PROJECT_SOURCE_DIR}/src/platform/kde/protocols/wlr-data-control-unstable-v1.xml"
  "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME wayland-wlr-data-control-unstable-v1.xml)
putgd_install_snapshot("${PROJECT_SOURCE_DIR}/src/platform/kde/protocols/zkde-screencast-unstable-v1.xml"
  "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME wayland-zkde-screencast-unstable-v1.xml)
# SDK license must come from the exact source tree provisioned by the build contract.
set(PUTGD_AWS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/build/dependencies/aws-sdk-cpp" CACHE PATH "Pinned official SDK source")
if(NOT EXISTS "${PUTGD_AWS_SOURCE_DIR}/LICENSE")
  message(FATAL_ERROR "Pinned SDK LICENSE required for packaging")
endif()
putgd_install_snapshot("${PUTGD_AWS_SOURCE_DIR}/LICENSE" "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME aws-sdk-LICENSE)
foreach(aws_component aws-crt-cpp aws-crt-cpp/crt/aws-c-auth aws-crt-cpp/crt/aws-c-cal
    aws-crt-cpp/crt/aws-c-common aws-crt-cpp/crt/aws-c-compression aws-crt-cpp/crt/aws-c-event-stream
    aws-crt-cpp/crt/aws-c-http aws-crt-cpp/crt/aws-c-io aws-crt-cpp/crt/aws-c-mqtt
    aws-crt-cpp/crt/aws-c-s3 aws-crt-cpp/crt/aws-c-sdkutils aws-crt-cpp/crt/aws-checksums
    aws-crt-cpp/crt/s2n)
  if(EXISTS "${PUTGD_AWS_SOURCE_DIR}/crt/${aws_component}/LICENSE")
    string(REPLACE "/" "-" license_name "${aws_component}")
    putgd_install_snapshot("${PUTGD_AWS_SOURCE_DIR}/crt/${aws_component}/LICENSE"
      "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME "${license_name}-LICENSE")
  else()
    message(FATAL_ERROR "Pinned SDK component LICENSE missing: ${aws_component}")
  endif()
  # Apache-2.0 4(d): the NOTICE files travel with the libraries (aws-checksums has none).
  if(EXISTS "${PUTGD_AWS_SOURCE_DIR}/crt/${aws_component}/NOTICE")
    putgd_install_snapshot("${PUTGD_AWS_SOURCE_DIR}/crt/${aws_component}/NOTICE"
      "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME "${license_name}-NOTICE")
  endif()
endforeach()
# The SDK and CRT compile in cJSON, TinyXML-2, libcbor and xxHash (#383), whose
# MIT, Zlib and BSD terms ask for their notices in binary copies too.
putgd_install_snapshot("${PUTGD_AWS_SOURCE_DIR}/NOTICE.txt" "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME aws-sdk-NOTICE)
putgd_install_snapshot("${PUTGD_AWS_SOURCE_DIR}/crt/aws-crt-cpp/crt/aws-c-common/THIRD-PARTY-LICENSES.txt"
  "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME aws-crt-cpp-crt-aws-c-common-THIRD-PARTY-LICENSES)
foreach(embedded IN ITEMS "src/aws-cpp-sdk-core/source/external/cjson/cJSON.cpp|aws-sdk-cJSON-LICENSE"
    "src/aws-cpp-sdk-core/source/external/tinyxml2/tinyxml2.cpp|aws-sdk-TinyXML2-LICENSE"
    "crt/aws-crt-cpp/crt/aws-checksums/source/external/xxhash.h|aws-crt-cpp-crt-aws-checksums-xxHash-LICENSE")
  string(REPLACE "|" ";" embedded "${embedded}")
  list(GET embedded 0 embedded_source)
  list(GET embedded 1 embedded_name)
  # Their licence is the source file's leading comment.
  file(READ "${PUTGD_AWS_SOURCE_DIR}/${embedded_source}" embedded_text LIMIT 4096)
  string(FIND "${embedded_text}" "*/" embedded_end)
  if(NOT embedded_text MATCHES "^/\\*" OR embedded_end LESS 0 OR NOT embedded_text MATCHES "[Cc]opyright|[Pp]ermission")
    message(FATAL_ERROR "Pinned SDK ${embedded_source} lost its leading licence comment")
  endif()
  math(EXPR embedded_end "${embedded_end} + 2")
  string(SUBSTRING "${embedded_text}" 0 ${embedded_end} embedded_text)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/package-inputs/aws/${embedded_name}"
    "From ${embedded_source} in the AWS SDK for C++ source:\n\n${embedded_text}\n")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/package-inputs/aws/${embedded_name}" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/putgd")
endforeach()
putgd_install_snapshot("${PROJECT_SOURCE_DIR}/LICENSE" "${CMAKE_INSTALL_DATADIR}/doc/putgd")

# Tagged DEB/RPM builds add their channel's signed repository at pkg.put.gd
# (#293), trusting only that channel's key. apt keeps a deleted conffile deleted
# and purge removes it; the RPM copies its file in on the first install only.
# Both channels' files are installed under /usr/share/putgd/repo too: moving
# to the other channel is copying one into place by hand (#386).
set(PUTGD_REPO_CHANNEL "")
if(PUTGD_RELEASE_TAG MATCHES "^v[0-9]+[.][0-9]+[.][0-9]+-nightly$")
  set(PUTGD_REPO_CHANNEL nightly)
elseif(PUTGD_RELEASE_TAG MATCHES "^v[0-9]+[.][0-9]+[.][0-9]+$")
  set(PUTGD_REPO_CHANNEL stable)
endif()
set(repo_dir "${CMAKE_CURRENT_SOURCE_DIR}/repo")
include("${PROJECT_SOURCE_DIR}/cmake/PackageRepositoryFiles.cmake")
if(PUTGD_REPO_CHANNEL AND PUTGD_PACKAGE_TARGET MATCHES "^(ubuntu24[.]04|ubuntu26[.]04|debian13|fedora44)$")
  putgd_repository_files("${PUTGD_PACKAGE_TARGET}" "${PUTGD_PACKAGE_REPOSITORY_URL}" "${repo_dir}" "${CMAKE_CURRENT_BINARY_DIR}/repo")
endif()
if(PUTGD_REPO_CHANNEL AND PUTGD_PACKAGE_TARGET MATCHES "^(ubuntu24[.]04|ubuntu26[.]04|debian13)$")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/repo/putgd-${PUTGD_REPO_CHANNEL}.sources" DESTINATION /etc/apt/sources.list.d RENAME putgd.sources)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/repo/putgd-nightly.sources" "${CMAKE_CURRENT_BINARY_DIR}/repo/putgd-stable.sources"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/putgd/repo")
  install(FILES "${repo_dir}/keys/putgd-${PUTGD_REPO_CHANNEL}.gpg" DESTINATION "${CMAKE_INSTALL_DATADIR}/keyrings"
    RENAME putgd-archive-keyring.gpg)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/repo/conffiles" "/etc/apt/sources.list.d/putgd.sources\n")
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_BINARY_DIR}/repo/conffiles")
  # 0644 whatever the builder's umask, as dpkg and lintian expect.
  set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
elseif(PUTGD_REPO_CHANNEL AND PUTGD_PACKAGE_TARGET STREQUAL "fedora44")
  # rpm-post.sh copies putgd.repo, this package's own channel, on the first install.
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/repo/putgd-${PUTGD_REPO_CHANNEL}.repo" DESTINATION "${CMAKE_INSTALL_DATADIR}/putgd/repo" RENAME putgd.repo)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/repo/putgd-nightly.repo" "${CMAKE_CURRENT_BINARY_DIR}/repo/putgd-stable.repo"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/putgd/repo")
  # Each channel's file names its own key, so either can be copied into place.
  # RPM-GPG-KEY-putgd, this package's channel, is what repository files
  # written before #386 name.
  install(FILES "${repo_dir}/keys/putgd-${PUTGD_REPO_CHANNEL}.asc" DESTINATION /etc/pki/rpm-gpg RENAME RPM-GPG-KEY-putgd)
  install(FILES "${repo_dir}/keys/putgd-nightly.asc" DESTINATION /etc/pki/rpm-gpg RENAME RPM-GPG-KEY-putgd-nightly)
  install(FILES "${repo_dir}/keys/putgd-stable.asc" DESTINATION /etc/pki/rpm-gpg RENAME RPM-GPG-KEY-putgd-stable)
  set(CPACK_RPM_USER_FILELIST "%ghost %config(noreplace) %attr(0644,root,root) /etc/yum.repos.d/putgd.repo")
  set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${repo_dir}/rpm-post.sh")
  set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${repo_dir}/rpm-postun.sh")
  list(APPEND putgd_rpm_system_dirs /etc/pki /etc/pki/rpm-gpg /etc/yum.repos.d)
endif()
set(CPACK_GENERATOR DEB) # Select -G RPM for a native Fedora build.
set(CPACK_PACKAGE_NAME putgd)
# package-identity.cmake sets the build identity's version when CPack runs.
set(CPACK_PACKAGE_VERSION "0.0.0")
configure_file("${PROJECT_SOURCE_DIR}/cmake/PackageBuildIdentity.cmake.in" package-identity.cmake @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/package-identity.cmake")
set(CPACK_PACKAGE_CONTACT "put.gd maintainers <packages@put.gd>")
set(CPACK_PACKAGE_VENDOR "put.gd")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Linux screen and clipboard sharing")
# The DEB's extended description and the RPM's %description (CPack's default is
# its own installer boilerplate). Same text as the AppStream summary.
set(CPACK_PACKAGE_DESCRIPTION "Capture and share screen regions, windows, the desktop, the clipboard and
screen recordings. put.gd uploads them to your own S3-compatible bucket or to
Proton Drive and puts the public link on the clipboard. It runs on GNOME, KDE
Plasma and Cinnamon, on Wayland and X11.

The package's licence metadata names its main licences. Every licence and
notice of the code it contains is listed in
/usr/share/doc/putgd/runtime-notices.txt.")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_PRIVATE_DIRS "${PUTGD_AWS_RUNTIME_DIR}")
# History previews of WebP recordings and images read through Qt's WebP image
# plugin (#189), which Debian, Ubuntu and Fedora ship apart from Qt GUI (GIF and
# JPEG come with it). The AppImage and the Flatpak runtime carry it already.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite, qt6-qpa-plugins, qt6-wayland, qt6-image-formats-plugins, libglib2.0-bin")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
# Resolve system SONAMEs on Fedora; Qt's platform/SQL plugins are loaded at runtime.
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
# The main licences: put.gd (MIT) and the private AWS SDK (Apache-2.0), plus
# the Proton helper's crypto below. runtime-notices.txt lists every other
# licence of the shipped code (#469, Patrick's decision).
set(CPACK_RPM_PACKAGE_LICENSE "MIT AND Apache-2.0")
set(CPACK_RPM_PACKAGE_URL "https://github.com/patrick-hudson/put.gd-desktop")
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-gui, qt6-qtbase, qt6-qtwayland, qt6-qtimageformats, glib2")
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
# Directories the filesystem, hicolor-icon-theme and appstream packages own.
list(APPEND putgd_rpm_system_dirs /usr/libexec /usr/share/applications /usr/share/metainfo /usr/share/icons
  /usr/share/icons/hicolor /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps)
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION ${putgd_rpm_system_dirs})
# Private AWS libraries satisfy only this app, never another RPM. Keep automatic
# detection of their system dependencies; filter only the bundled SONAMEs.
# RPM's default stripping also changes bytes after our runtime digest is written.
set(CPACK_RPM_SPEC_MORE_DEFINE
  "%global __provides_exclude_from ^${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/putgd/.*$\n%global __requires_exclude ^(libaws[-A-Za-z0-9_]*|libs2n)[.]so.*$\n%define __spec_install_post /bin/true")
if(TARGET putgd-record)
  # The helper links the distro GStreamer/FFmpeg libraries (shlibdeps/RPM find
  # those); element plugins are separate packages that must be present.
  # Debian/Ubuntu: plugins-bad carries openh264enc and voaacenc (and pulls
  # plugins-extra, which holds the va encoders on Ubuntu 26.04), so a live
  # H.264 and AAC encoder are always there; x264 and VA-API are preferred.
  string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-pipewire")
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gstreamer1.0-plugins-ugly, gstreamer1.0-libav, gstreamer1.0-vaapi")
  # Fedora: bad-free carries the va and fdkaacenc elements, but its stock Intel
  # and AMD drivers have no H.264 encoding, so openh264enc is the only live
  # encoder; it and FFmpeg load libopenh264, which is the noopenh264
  # placeholder unless Cisco's openh264 (fedora-cisco-openh264) replaces it.
  string(APPEND CPACK_RPM_PACKAGE_REQUIRES ", gstreamer1-plugins-base, gstreamer1-plugins-good, gstreamer1-plugins-bad-free, pipewire-gstreamer")
  set(CPACK_RPM_PACKAGE_RECOMMENDS "gstreamer1-plugin-openh264, openh264")
endif()
if(PUTGD_BUNDLE_PROTON)
  string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", xdg-utils, libsecret-1-0")
  string(APPEND CPACK_RPM_PACKAGE_REQUIRES ", xdg-utils, libsecret")
  # WebPackages' README grants its crypto "version 3 ... or (at your option)
  # any later version" (956a713c); package.json's bare GPL-3.0 says no more.
  string(APPEND CPACK_RPM_PACKAGE_LICENSE " AND GPL-3.0-or-later")
  # The helper's own pointer to the source published with this release.
  configure_file(proton-source-location.txt.in package-inputs/proton/SOURCE-LOCATION.txt @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/package-inputs/proton/SOURCE-LOCATION.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/putgd/proton")
endif()
# AppStream: software centres show put.gd and its updates as an application.
# The release entry is this build's package version and commit date.
set(PUTGD_METAINFO_LICENSE "${CPACK_RPM_PACKAGE_LICENSE}")
set(PUTGD_METAINFO_RELEASES "")
file(READ "${PUTGD_BUILD_IDENTITY_DIR}/build-info.json" metainfo_identity)
string(JSON metainfo_version GET "${metainfo_identity}" packageVersion)
string(JSON metainfo_commit GET "${metainfo_identity}" gitCommit)
find_program(PUTGD_GIT git)
if(PUTGD_GIT AND metainfo_commit MATCHES "^[0-9a-f]+$" AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  # The UTC date, as in the version (#373).
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env TZ=UTC0 "${PUTGD_GIT}" -C "${PROJECT_SOURCE_DIR}" show -s
      --date=format-local:%Y-%m-%d --format=%cd "${metainfo_commit}"
    OUTPUT_VARIABLE metainfo_date OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE metainfo_result ERROR_QUIET)
  if(metainfo_result EQUAL 0 AND metainfo_date MATCHES "^[0-9]+-[0-9]+-[0-9]+$")
    set(PUTGD_METAINFO_RELEASES "  <releases>\n    <release version=\"${metainfo_version}\" date=\"${metainfo_date}\"/>\n  </releases>\n")
  endif()
endif()
configure_file(gd.put.Putgd.metainfo.xml.in gd.put.Putgd.metainfo.xml @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/gd.put.Putgd.metainfo.xml" DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
