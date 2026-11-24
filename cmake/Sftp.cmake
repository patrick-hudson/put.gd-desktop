# SFTP destination (#1871). putgd_sftp_rules holds the settings rules the resident, the
# Settings form and the worker share; putgd_sftp_wire is the SFTP v3 packet format the
# worker and its fuzz target read. The resident links putgd_sftp; every request runs in
# putgd-sftp-worker, which runs the user's own ssh and is the only target with Qt
# Network for it. putgd-sftp-askpass is ssh's SSH_ASKPASS program: C library only, so it
# starts under the host's library path from the AppImage too.
add_library(putgd_sftp_rules STATIC src/storage/sftp/SftpCommon.cpp)
set_target_properties(putgd_sftp_rules PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(putgd_sftp_rules PUBLIC src)
target_link_libraries(putgd_sftp_rules PUBLIC putgd_build_options Qt6::Core)
add_library(putgd_sftp_wire STATIC src/storage/sftp/SftpWire.cpp)
target_include_directories(putgd_sftp_wire PUBLIC src)
target_link_libraries(putgd_sftp_wire PUBLIC putgd_build_options Qt6::Core)
add_library(putgd_sftp STATIC src/storage/sftp/SftpProvider.cpp)
target_link_libraries(putgd_sftp PUBLIC putgd_sftp_rules putgd_core putgd_credentials PRIVATE Qt6::Core Threads::Threads)
add_executable(putgd-sftp-worker src/storage/sftp/SftpWorkerMain.cpp src/storage/sftp/SftpSsh.cpp src/storage/sftp/SftpClient.cpp)
target_link_libraries(putgd-sftp-worker PRIVATE putgd_sftp_rules putgd_sftp_wire putgd_core putgd_build_identity Qt6::Network Threads::Threads)
add_executable(putgd-sftp-askpass src/storage/sftp/SftpAskpass.c)
target_compile_definitions(putgd_sftp PRIVATE
  PUTGD_SFTP_BUILD_APP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  PUTGD_SFTP_WORKER_PATH="$<TARGET_FILE:putgd-sftp-worker>")
target_compile_definitions(putgd-sftp-worker PRIVATE
  PUTGD_SFTP_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  PUTGD_SFTP_ASKPASS_PATH="$<TARGET_FILE:putgd-sftp-askpass>")
add_dependencies(putgd_sftp putgd-sftp-worker putgd-sftp-askpass)
target_link_libraries(putgd PRIVATE putgd_sftp)
target_link_libraries(putgd_ui PRIVATE putgd_sftp_rules)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(sftp_worker_identity_check [=[
    file(READ "@PUTGD_BUILD_IDENTITY_DIR@/build-info.json" expected_sftp_manifest)
    execute_process(COMMAND "$<TARGET_FILE:putgd-sftp-worker>" --version-json
      RESULT_VARIABLE sftp_identity_result OUTPUT_VARIABLE sftp_manifest
      ERROR_VARIABLE sftp_identity_error OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
    if(NOT sftp_identity_result EQUAL 0)
      message(FATAL_ERROR "Build the SFTP worker before installation or packaging")
    endif()
    string(JSON expected_sftp_identity GET "${expected_sftp_manifest}")
    string(JSON actual_sftp_identity GET "${sftp_manifest}")
    if(NOT actual_sftp_identity STREQUAL expected_sftp_identity)
      message(FATAL_ERROR "SFTP worker does not match the compiled build manifest; build all product executables")
    endif()
  ]=])
  string(CONFIGURE "${sftp_worker_identity_check}" sftp_worker_identity_check @ONLY)
  install(CODE "${sftp_worker_identity_check}")
  install(TARGETS putgd-sftp-worker putgd-sftp-askpass RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
