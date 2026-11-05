# Backblaze B2 native destination (#282). The resident links putgd_b2 (Qt Core only);
# every B2 request runs in putgd-b2-worker, the only target with Qt Network for it.
add_library(putgd_b2 STATIC src/storage/b2/B2Common.cpp src/storage/b2/B2Provider.cpp)
target_link_libraries(putgd_b2 PUBLIC putgd_core putgd_credentials PRIVATE Qt6::Core)
add_executable(putgd-b2-worker src/storage/b2/B2WorkerMain.cpp src/storage/b2/B2Common.cpp)
target_link_libraries(putgd-b2-worker PRIVATE putgd_core putgd_build_identity Qt6::Network Threads::Threads)
target_compile_definitions(putgd_b2 PRIVATE
  PUTGD_B2_BUILD_APP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  PUTGD_B2_WORKER_PATH="$<TARGET_FILE:putgd-b2-worker>")
add_dependencies(putgd_b2 putgd-b2-worker)
target_link_libraries(putgd PRIVATE putgd_b2)
target_link_libraries(putgd_ui PRIVATE putgd_b2)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(b2_worker_identity_check [=[
    file(READ "@PUTGD_BUILD_IDENTITY_DIR@/build-info.json" expected_b2_manifest)
    execute_process(COMMAND "$<TARGET_FILE:putgd-b2-worker>" --version-json
      RESULT_VARIABLE b2_identity_result OUTPUT_VARIABLE b2_manifest
      ERROR_VARIABLE b2_identity_error OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
    if(NOT b2_identity_result EQUAL 0)
      message(FATAL_ERROR "Build the B2 worker before installation or packaging")
    endif()
    string(JSON expected_b2_identity GET "${expected_b2_manifest}")
    string(JSON actual_b2_identity GET "${b2_manifest}")
    if(NOT actual_b2_identity STREQUAL expected_b2_identity)
      message(FATAL_ERROR "B2 worker does not match the compiled build manifest; build all product executables")
    endif()
  ]=])
  string(CONFIGURE "${b2_worker_identity_check}" b2_worker_identity_check @ONLY)
  install(CODE "${b2_worker_identity_check}")
  install(TARGETS putgd-b2-worker RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
