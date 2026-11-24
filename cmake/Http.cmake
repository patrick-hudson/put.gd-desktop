# Custom HTTP uploader destination (#1361). putgd_http_rules holds the settings rules,
# the .sxcu reader and the placeholder engine (Qt Core only), shared by the resident,
# the Settings form and the worker. The resident links putgd_http; every request runs
# in putgd-http-worker, the only target with Qt Network for it.
add_library(putgd_http_rules STATIC src/storage/http/HttpCommon.cpp src/storage/http/HttpTemplate.cpp)
set_target_properties(putgd_http_rules PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(putgd_http_rules PUBLIC src)
target_link_libraries(putgd_http_rules PUBLIC putgd_build_options Qt6::Core)
add_library(putgd_http STATIC src/storage/http/HttpProvider.cpp src/storage/http/HttpImport.cpp)
target_link_libraries(putgd_http PRIVATE putgd_provider_exchange PUBLIC putgd_http_rules putgd_core putgd_credentials PRIVATE Qt6::Core)
add_executable(putgd-http-worker src/storage/http/HttpWorkerMain.cpp)
target_link_libraries(putgd-http-worker PRIVATE putgd_worker_stdio putgd_http_rules putgd_core putgd_build_identity Qt6::Network Threads::Threads)
target_compile_definitions(putgd_http PRIVATE
  PUTGD_HTTP_BUILD_APP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  PUTGD_HTTP_WORKER_PATH="$<TARGET_FILE:putgd-http-worker>")
add_dependencies(putgd_http putgd-http-worker)
target_link_libraries(putgd PRIVATE putgd_http)
target_link_libraries(putgd_ui PRIVATE putgd_http_rules)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(http_worker_identity_check [=[
    file(READ "@PUTGD_BUILD_IDENTITY_DIR@/build-info.json" expected_http_manifest)
    execute_process(COMMAND "$<TARGET_FILE:putgd-http-worker>" --version-json
      RESULT_VARIABLE http_identity_result OUTPUT_VARIABLE http_manifest
      ERROR_VARIABLE http_identity_error OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
    if(NOT http_identity_result EQUAL 0)
      message(FATAL_ERROR "Build the custom uploader worker before installation or packaging")
    endif()
    string(JSON expected_http_identity GET "${expected_http_manifest}")
    string(JSON actual_http_identity GET "${http_manifest}")
    if(NOT actual_http_identity STREQUAL expected_http_identity)
      message(FATAL_ERROR "Custom uploader worker does not match the compiled build manifest; build all product executables")
    endif()
  ]=])
  string(CONFIGURE "${http_worker_identity_check}" http_worker_identity_check @ONLY)
  install(CODE "${http_worker_identity_check}")
  install(TARGETS putgd-http-worker RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
