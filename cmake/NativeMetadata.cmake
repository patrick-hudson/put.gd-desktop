# After Proton's install rules: the retained copyright file carries all notices,
# including those lost on Debian installs with path-exclude=/usr/share/doc/*.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(native_metadata_time "${PUTGD_SOURCE_COMMIT_TIME}")
if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
  execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}" show -s --format=%ct HEAD
    OUTPUT_VARIABLE native_metadata_time OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()
if(NOT native_metadata_time MATCHES "^[0-9]+$")
  # Unknown, unversioned local source still has deterministic metadata.
  set(native_metadata_time 0)
endif()
set(native_metadata_code [=[
  execute_process(COMMAND "@Python3_EXECUTABLE@" "@PROJECT_SOURCE_DIR@/tools/releases/native_metadata.py"
    --docs "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/@CMAKE_INSTALL_DATADIR@/doc/putgd"
    --identity "@PUTGD_BUILD_IDENTITY_DIR@/build-info.json" --source-url "@PUTGD_SOURCE_URL@"
    --timestamp "@native_metadata_time@" COMMAND_ERROR_IS_FATAL ANY)
]=])
string(CONFIGURE "${native_metadata_code}" native_metadata_code @ONLY)
install(CODE "${native_metadata_code}")
# CPack removes exact paths from its generated list. Enumerate every notice;
# wildcard entries would leave the automatic %doc entries in place.
set(native_license_names LICENSE runtime-notices.txt copyright wayland-ext-data-control-v1.xml
  wayland-wlr-data-control-unstable-v1.xml wayland-zkde-screencast-unstable-v1.xml)
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
include(CPack)
