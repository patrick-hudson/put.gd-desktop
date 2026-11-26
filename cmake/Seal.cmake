# Sealed links (#2079). The viewer page's exact bytes are compiled in; the crypto links
# OpenSSL and only putgd-cli (and tests) link it: the resident runs `putgd-cli --seal-job`.
set(putgd_seal_viewer "${CMAKE_CURRENT_SOURCE_DIR}/src/seal/viewer.html")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${putgd_seal_viewer}")
file(READ "${putgd_seal_viewer}" putgd_seal_viewer_hex HEX)
file(SIZE "${putgd_seal_viewer}" putgd_seal_viewer_size)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," putgd_seal_viewer_bytes "${putgd_seal_viewer_hex}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/seal/SealViewerBytes.cpp" CONTENT
"// Generated from src/seal/viewer.html by cmake/Seal.cmake.
namespace putgd::seal {
extern const unsigned char sealViewerBytes[]={${putgd_seal_viewer_bytes}};
extern const unsigned long long sealViewerSize=${putgd_seal_viewer_size};
}
" @ONLY)
# Qt Core only: page assembly and checking, no crypto.
add_library(putgd_seal_page STATIC src/seal/SealPage.cpp "${CMAKE_BINARY_DIR}/generated/seal/SealViewerBytes.cpp")
target_include_directories(putgd_seal_page PUBLIC src)
target_link_libraries(putgd_seal_page PUBLIC Qt6::Core putgd_build_options)
add_library(putgd_seal STATIC src/seal/SealCrypto.cpp src/seal/SealJobMain.cpp)
target_include_directories(putgd_seal PUBLIC src)
target_link_libraries(putgd_seal PUBLIC putgd_seal_page Qt6::Core putgd_build_options PRIVATE OpenSSL::Crypto)
