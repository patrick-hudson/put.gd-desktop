# The S3 destination: its wire protocol, provider, SDK module and worker.
# The resident facade and credential cache carry no SDK link requirement.
add_library(putgd_s3_wire STATIC src/storage/s3/S3WorkerProtocol.cpp src/storage/s3/IpAddress.cpp src/storage/s3/PublicUrl.cpp)
target_link_libraries(putgd_s3_wire PUBLIC putgd_core)
# Non-Linux uses QHostAddress; the Linux resident keeps its native parser.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(putgd_s3_wire PRIVATE Qt6::Network)
endif()
add_library(putgd_s3 STATIC src/storage/s3/S3Provider.cpp src/storage/s3/S3Module.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(putgd_s3 PRIVATE src/storage/s3/S3ProcessDriver.cpp)
endif()
target_link_libraries(putgd_s3 PUBLIC putgd_core putgd_credentials putgd_s3_wire PRIVATE Threads::Threads putgd_ui_control)
add_library(putgd_s3_module MODULE src/storage/s3/S3Module.cpp src/storage/s3/S3Driver.cpp
  src/storage/s3/S3SdkContext.cpp src/storage/s3/PublicUrl.cpp)
target_link_libraries(putgd_s3_module PRIVATE putgd_core putgd_s3_runtime ${AWSSDK_LINK_LIBRARIES} Threads::Threads Qt6::Network)
set_target_properties(putgd_s3_module PROPERTIES OUTPUT_NAME putgd-s3
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(putgd_s3_module PRIVATE "LINKER:--exclude-libs,ALL")
endif()
# Private C++/Qt ABI requires this exact source, toolchain and transport contract.
set(s3_contract "${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${Qt6_VERSION};${AWSSDK_VERSION};${CMAKE_BUILD_TYPE};${PUTGD_SANITIZER};${PUTGD_SDK_CURL_TRANSPORT};${OpenSSL_FOUND};${OPENSSL_VERSION};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_DEBUG};${CMAKE_CXX_FLAGS_RELEASE}")
foreach(s3_contract_file src/storage/s3/S3Driver.h src/storage/s3/S3Driver.cpp
    src/storage/s3/S3ProcessDriver.h src/storage/s3/S3ProcessDriver.cpp
    src/storage/s3/S3WorkerProtocol.h src/storage/s3/S3WorkerProtocol.cpp src/storage/s3/S3WorkerMain.cpp
    src/storage/s3/S3Module.h src/storage/s3/S3Module.cpp src/storage/s3/S3Provider.cpp src/storage/s3/S3Provider.h src/storage/s3/S3Multipart.h src/storage/s3/S3Journal.h src/storage/s3/IpAddress.cpp src/storage/s3/IpAddress.h
    src/storage/s3/S3SdkContext.h src/storage/s3/S3SdkContext.cpp src/storage/s3/SdkRuntime.h
    src/storage/s3/SdkRuntime.cpp src/storage/s3/TlsCompatibility.h src/storage/s3/TlsCompatibility.cpp src/storage/s3/PublicUrl.h src/storage/s3/PublicUrl.cpp
    src/core/StorageProvider.h src/core/StorageReceipt.h src/core/ArtifactStore.h
    src/core/BuildInfo.h src/core/Settings.h src/ipc/UiControl.h
    src/core/Error.h src/core/ClipboardPayload.h src/secrets/CredentialStore.h cmake/dependency-lock.json)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${s3_contract_file}")
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${s3_contract_file}" s3_contract_hash)
  string(APPEND s3_contract "${s3_contract_hash}")
endforeach()
string(SHA256 PUTGD_S3_BUILD_ID "${s3_contract}")
if(PUTGD_SDK_CURL_TRANSPORT)
  set(s3_safe_delete 1)
else()
  set(s3_safe_delete 0)
endif()
foreach(s3_target putgd_s3 putgd_s3_module)
  target_compile_definitions(${s3_target} PRIVATE PUTGD_S3_BUILD_ID="${PUTGD_S3_BUILD_ID}"
    PUTGD_SDK_CURL_TRANSPORT=${s3_safe_delete})
endforeach()
target_compile_definitions(putgd_s3_module PRIVATE PUTGD_S3_MODULE_EXPORTS=1)
file(RELATIVE_PATH s3_install_relative "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
  "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/putgd/${CMAKE_SHARED_MODULE_PREFIX}putgd-s3${CMAKE_SHARED_MODULE_SUFFIX}")
target_compile_definitions(putgd_s3 PRIVATE
  PUTGD_S3_BUILD_APP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  PUTGD_S3_BUILD_MODULE_PATH="$<TARGET_FILE:putgd_s3_module>"
  PUTGD_S3_INSTALL_MODULE_RELATIVE="${s3_install_relative}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(putgd_s3_worker_host STATIC src/storage/s3/S3Module.cpp)
  target_link_libraries(putgd_s3_worker_host PUBLIC putgd_core PRIVATE Threads::Threads)
  target_compile_definitions(putgd_s3_worker_host PRIVATE PUTGD_S3_WORKER_HOST=1
    PUTGD_S3_BUILD_ID="${PUTGD_S3_BUILD_ID}" PUTGD_SDK_CURL_TRANSPORT=${s3_safe_delete}
    PUTGD_S3_BUILD_APP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    PUTGD_S3_BUILD_MODULE_PATH="$<TARGET_FILE:putgd_s3_module>"
    PUTGD_S3_INSTALL_MODULE_RELATIVE="${s3_install_relative}")
  add_executable(putgd-s3-worker src/storage/s3/S3WorkerMain.cpp)
  target_link_libraries(putgd-s3-worker PRIVATE putgd_s3_worker_host putgd_s3_wire putgd_ui_control putgd_build_identity)
  target_compile_definitions(putgd-s3-worker PRIVATE PUTGD_S3_BUILD_ID="${PUTGD_S3_BUILD_ID}" PUTGD_SDK_CURL_TRANSPORT=${s3_safe_delete})
  target_compile_definitions(putgd_s3 PRIVATE PUTGD_S3_WORKER_PATH="$<TARGET_FILE:putgd-s3-worker>")
  add_dependencies(putgd-s3-worker putgd_s3_module)
  add_dependencies(putgd_s3 putgd-s3-worker)
  set(worker_identity_check [=[
    file(READ "@PUTGD_BUILD_IDENTITY_DIR@/build-info.json" expected_worker_manifest)
    execute_process(COMMAND "$<TARGET_FILE:putgd-s3-worker>" --version-json
      RESULT_VARIABLE worker_identity_result OUTPUT_VARIABLE worker_manifest
      ERROR_VARIABLE worker_identity_error OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
    if(NOT worker_identity_result EQUAL 0)
      message(FATAL_ERROR "Build the S3 worker before installation or packaging")
    endif()
    string(JSON expected_worker_identity GET "${expected_worker_manifest}")
    string(JSON actual_worker_identity GET "${worker_manifest}")
    if(NOT actual_worker_identity STREQUAL expected_worker_identity)
      message(FATAL_ERROR "S3 worker does not match the compiled build manifest; build all product executables")
    endif()
  ]=])
  string(CONFIGURE "${worker_identity_check}" worker_identity_check @ONLY)
  install(CODE "${worker_identity_check}")
  install(TARGETS putgd-s3-worker RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
add_dependencies(putgd_s3 putgd_s3_module)
install(TARGETS putgd_s3_module LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/putgd")
