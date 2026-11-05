# The AWS SDK runtime library and the probe for its Curl transport.
add_library(putgd_s3_runtime STATIC src/storage/s3/SdkRuntime.cpp src/storage/s3/TlsCompatibility.cpp)
set_target_properties(putgd_s3_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(putgd_s3_runtime PUBLIC src)
target_link_libraries(putgd_s3_runtime PRIVATE ${AWSSDK_LINK_LIBRARIES} PUBLIC putgd_build_options)
# The SDK does not export its HTTP backend compile flags. Probe the actual installed
# SDK Curl implementation; retain the default factory on non-Curl SDK builds.
find_package(CURL QUIET)
if(CURL_FOUND)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_LIBRARIES ${AWSSDK_LINK_LIBRARIES} CURL::libcurl)
  check_cxx_source_compiles("#include <aws/core/http/curl/CurlHttpClient.h>
int main() { Aws::Http::CurlHttpClient::InitGlobalState(); Aws::Http::CurlHttpClient::CleanupGlobalState(); }" PUTGD_SDK_CURL_TRANSPORT)
  unset(CMAKE_REQUIRED_LIBRARIES)
  if(PUTGD_SDK_CURL_TRANSPORT)
    target_compile_definitions(putgd_s3_runtime PRIVATE PUTGD_SDK_CURL_TRANSPORT=1)
    target_link_libraries(putgd_s3_runtime PRIVATE CURL::libcurl)
    find_package(OpenSSL QUIET COMPONENTS Crypto)
    if(OpenSSL_FOUND)
      target_compile_definitions(putgd_s3_runtime PUBLIC PUTGD_OPENSSL_GROUP_POLICY=1)
      target_link_libraries(putgd_s3_runtime PRIVATE OpenSSL::Crypto)
    endif()
  endif()
endif()
