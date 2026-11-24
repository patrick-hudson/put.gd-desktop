# Nextcloud and WebDAV destinations (#1872). putgd_dav_rules holds the settings and URL
# rules (Qt Core only), shared by the resident, the Settings form and the worker. The
# resident links putgd_dav; every request runs in putgd-webdav-worker, the only target
# with Qt Network for it.
add_library(putgd_dav_rules STATIC src/storage/dav/DavCommon.cpp)
set_target_properties(putgd_dav_rules PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(putgd_dav_rules PUBLIC src)
target_link_libraries(putgd_dav_rules PUBLIC putgd_build_options putgd_http_rules Qt6::Core)
add_library(putgd_dav STATIC src/storage/dav/DavProvider.cpp)
target_link_libraries(putgd_dav PRIVATE putgd_provider_exchange PUBLIC putgd_dav_rules putgd_core putgd_credentials PRIVATE Qt6::Core)
add_executable(putgd-webdav-worker src/storage/dav/DavWorkerMain.cpp)
target_link_libraries(putgd-webdav-worker PRIVATE putgd_worker_stdio putgd_dav_rules putgd_core putgd_build_identity Qt6::Network Threads::Threads)
target_compile_definitions(putgd_dav PRIVATE
  PUTGD_DAV_BUILD_APP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  PUTGD_DAV_WORKER_PATH="$<TARGET_FILE:putgd-webdav-worker>")
add_dependencies(putgd_dav putgd-webdav-worker)
target_link_libraries(putgd PRIVATE putgd_dav)
target_link_libraries(putgd_ui PRIVATE putgd_dav_rules)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(dav_worker_identity_check [=[
    file(READ "@PUTGD_BUILD_IDENTITY_DIR@/build-info.json" expected_dav_manifest)
    execute_process(COMMAND "$<TARGET_FILE:putgd-webdav-worker>" --version-json
      RESULT_VARIABLE dav_identity_result OUTPUT_VARIABLE dav_manifest
      ERROR_VARIABLE dav_identity_error OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
    if(NOT dav_identity_result EQUAL 0)
      message(FATAL_ERROR "Build the WebDAV worker before installation or packaging")
    endif()
    string(JSON expected_dav_identity GET "${expected_dav_manifest}")
    string(JSON actual_dav_identity GET "${dav_manifest}")
    if(NOT actual_dav_identity STREQUAL expected_dav_identity)
      message(FATAL_ERROR "WebDAV worker does not match the compiled build manifest; build all product executables")
    endif()
  ]=])
  string(CONFIGURE "${dav_worker_identity_check}" dav_worker_identity_check @ONLY)
  install(CODE "${dav_worker_identity_check}")
  install(TARGETS putgd-webdav-worker RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
