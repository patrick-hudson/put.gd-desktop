# After Proton's install rules: the retained copyright file carries all notices,
# including those lost on Debian installs with path-exclude=/usr/share/doc/*.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_directory_property(PUTGD_SOURCE_URL DIRECTORY "${PROJECT_SOURCE_DIR}/packaging/linux" DEFINITION PUTGD_SOURCE_URL)
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
