# Refresh on every normal build; configure_file writes only when identity changes.
# This handles commits, index changes and working-tree edits without a reconfigure.
set(PUTGD_RELEASE_TAG "" CACHE STRING "Exact clean Git release tag: v<base>, v<base>-rc.N or v<base>-dev.COUNT.UTC+gSHORT")
set(PUTGD_SOURCE_COMMIT "" CACHE STRING "Full commit for an exported source archive")
set(PUTGD_SOURCE_TREE "" CACHE STRING "Full committed tree for an exported source archive")
set(PUTGD_SOURCE_COMMIT_COUNT "" CACHE STRING "Commit count for an exported source archive")
set(PUTGD_BUILD_IDENTITY_DIR "${PROJECT_BINARY_DIR}/build-identity")
set(identity_command "${CMAKE_COMMAND}"
  "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
  "-DOUTPUT_DIR=${PUTGD_BUILD_IDENTITY_DIR}"
  "-DBASE_VERSION=${PROJECT_VERSION}"
  "-DRELEASE_TAG=${PUTGD_RELEASE_TAG}"
  "-DSUPPLIED_COMMIT=${PUTGD_SOURCE_COMMIT}"
  "-DSUPPLIED_TREE=${PUTGD_SOURCE_TREE}"
  "-DSUPPLIED_COUNT=${PUTGD_SOURCE_COMMIT_COUNT}"
  -P "${CMAKE_CURRENT_LIST_DIR}/GenerateBuildInfo.cmake")
execute_process(COMMAND ${identity_command} RESULT_VARIABLE identity_result)
if(NOT identity_result EQUAL 0)
  message(FATAL_ERROR "Cannot generate build identity")
endif()
add_custom_target(putgd_refresh_build_identity
  COMMAND ${identity_command}
  BYPRODUCTS "${PUTGD_BUILD_IDENTITY_DIR}/BuildInfo.cpp" "${PUTGD_BUILD_IDENTITY_DIR}/build-info.json"
  VERBATIM)
add_library(putgd_build_identity STATIC "${PUTGD_BUILD_IDENTITY_DIR}/BuildInfo.cpp")
add_dependencies(putgd_build_identity putgd_refresh_build_identity)
target_compile_features(putgd_build_identity PUBLIC cxx_std_20)
target_include_directories(putgd_build_identity PUBLIC "${PROJECT_SOURCE_DIR}/src")
set_target_properties(putgd_build_identity PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Validate before any install rules write files. Resolving the target paths is
# deferred to generation, after the root has declared its product executables.
set(PUTGD_PACKAGE_APP "$<TARGET_FILE:putgd>")
set(PUTGD_PACKAGE_CLI "$<TARGET_FILE:putgd-cli>")
set(PUTGD_PACKAGE_UI "$<TARGET_FILE:putgd-ui>")
# The KDE selector is required only on builds that declare its target.
set(PUTGD_PACKAGE_CAPTURE "$<$<TARGET_EXISTS:putgd-capture>:$<TARGET_FILE:putgd-capture>>")
set(PUTGD_PACKAGE_KDE_REGION "$<$<TARGET_EXISTS:putgd-kde-region>:$<TARGET_FILE:putgd-kde-region>>")
set(PUTGD_PACKAGE_RECORD "$<$<TARGET_EXISTS:putgd-record>:$<TARGET_FILE:putgd-record>>")
configure_file("${CMAKE_CURRENT_LIST_DIR}/CheckBuildIdentity.cmake.in"
  "${PUTGD_BUILD_IDENTITY_DIR}/check-build-identity.in" @ONLY)
file(GENERATE OUTPUT "${PUTGD_BUILD_IDENTITY_DIR}/check-build-identity.cmake"
  INPUT "${PUTGD_BUILD_IDENTITY_DIR}/check-build-identity.in")
install(SCRIPT "${PUTGD_BUILD_IDENTITY_DIR}/check-build-identity.cmake")
install(FILES "${PUTGD_BUILD_IDENTITY_DIR}/build-info.json" DESTINATION "${CMAKE_INSTALL_DATADIR}/putgd")
