# CPack removes exact paths from its generated list. Enumerate every notice;
# wildcard entries would leave the automatic %doc entries in place.
set(native_license_names qrcodegen-LICENSE LICENSE LGPL-2.1.txt runtime-notices.txt copyright fonts/OFL-sans.txt fonts/OFL-mono.txt fonts/SOURCE.md wayland-ext-data-control-v1.xml
  wayland-wlr-data-control-unstable-v1.xml wayland-zkde-screencast-unstable-v1.xml
  wayland-ext-image-copy-capture-v1.xml wayland-ext-image-capture-source-v1.xml wayland-ext-foreign-toplevel-list-v1.xml wayland-wlr-foreign-toplevel-management-unstable-v1.xml
  wayland-hyprland-global-shortcuts-v1.xml wayland-hyprland-toplevel-mapping-v1.xml wayland-wlr-layer-shell-unstable-v1.xml
  wayland-wlr-virtual-pointer-unstable-v1.xml wayland-fake-input.xml)
file(GLOB native_aws_notices RELATIVE "${PROJECT_BINARY_DIR}/packaging/linux/package-inputs/${CMAKE_INSTALL_DATADIR}/doc/putgd"
  "${PROJECT_BINARY_DIR}/packaging/linux/package-inputs/${CMAKE_INSTALL_DATADIR}/doc/putgd/aws-*")
# Snapshot files and extracted leading comments are in two directories.
file(GLOB native_aws_extracted RELATIVE "${PROJECT_BINARY_DIR}/packaging/linux/package-inputs/aws"
  "${PROJECT_BINARY_DIR}/packaging/linux/package-inputs/aws/*")
list(APPEND native_license_names ${native_aws_notices} ${native_aws_extracted})
if(PUTGD_BUNDLE_PROTON)
  file(GLOB native_proton_notices RELATIVE "${PROJECT_SOURCE_DIR}/tools/proton/notices"
    "${PROJECT_SOURCE_DIR}/tools/proton/notices/*")
  foreach(name IN LISTS native_proton_notices)
    list(APPEND native_license_names "proton/${name}")
  endforeach()
  list(APPEND native_license_names proton/SOURCE-LOCATION.txt)
endif()
foreach(name IN LISTS native_license_names)
  list(APPEND CPACK_RPM_USER_FILELIST "%license ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/doc/putgd/${name}")
endforeach()
