# Root integrates this directory after defining putgd and putgd-cli.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
install(FILES putgd.svg DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
option(PUTGD_LAB_FIXTURE "Package independent synthetic desktop fixture (development only)" OFF)
configure_file(app.desktop.in putgd.desktop @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/putgd.desktop"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${PROJECT_SOURCE_DIR}/integrations/gnome/extension.js" "${PROJECT_SOURCE_DIR}/integrations/gnome/metadata.json"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/putgd@put.gd")
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
target_link_options(putgd PRIVATE "LINKER:--disable-new-dtags")
set_target_properties(putgd PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/putgd")
# Every SDK shared object also resolves its CRT siblings from this private directory.
# The binary uses DT_RPATH so SDK dependencies resolve transitively; the launcher
# also scopes the private directory to this process for explicit desktop startup.
configure_file(putgd-launcher.in putgd-launcher @ONLY)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/putgd-launcher" DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME putgd-launcher)
install(FILES "${PROJECT_SOURCE_DIR}/cmake/dependency-lock.json" DESTINATION "${CMAKE_INSTALL_DATADIR}/putgd")
install(FILES runtime-notices.txt DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/putgd")
install(FILES "${PROJECT_SOURCE_DIR}/src/platform/kde/protocols/ext-data-control-v1.xml"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME wayland-ext-data-control-v1.xml)
# SDK license must come from the exact source tree provisioned by the build contract.
set(PUTGD_AWS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/build/dependencies/aws-sdk-cpp" CACHE PATH "Pinned official SDK source")
if(NOT EXISTS "${PUTGD_AWS_SOURCE_DIR}/LICENSE")
  message(FATAL_ERROR "Pinned SDK LICENSE required for packaging")
endif()
install(FILES "${PUTGD_AWS_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME aws-sdk-LICENSE)
foreach(aws_component aws-crt-cpp aws-crt-cpp/crt/aws-c-auth aws-crt-cpp/crt/aws-c-cal
    aws-crt-cpp/crt/aws-c-common aws-crt-cpp/crt/aws-c-compression aws-crt-cpp/crt/aws-c-event-stream
    aws-crt-cpp/crt/aws-c-http aws-crt-cpp/crt/aws-c-io aws-crt-cpp/crt/aws-c-mqtt
    aws-crt-cpp/crt/aws-c-s3 aws-crt-cpp/crt/aws-c-sdkutils aws-crt-cpp/crt/aws-checksums
    aws-crt-cpp/crt/s2n)
  if(EXISTS "${PUTGD_AWS_SOURCE_DIR}/crt/${aws_component}/LICENSE")
    string(REPLACE "/" "-" license_name "${aws_component}")
    install(FILES "${PUTGD_AWS_SOURCE_DIR}/crt/${aws_component}/LICENSE"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/putgd" RENAME "${license_name}-LICENSE")
  else()
    message(FATAL_ERROR "Pinned SDK component LICENSE missing: ${aws_component}")
  endif()
endforeach()
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME putgd)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "put.gd maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Linux screen and clipboard sharing")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_PRIVATE_DIRS "${PUTGD_AWS_RUNTIME_DIR}")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite, qt6-qpa-plugins, qt6-wayland")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
