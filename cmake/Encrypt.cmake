# Encrypted links (#2079). The viewer page's exact bytes are compiled in; the crypto links
# OpenSSL and only putgd-cli (and tests) link it: the resident runs `putgd-cli --encrypt-job`.
set(putgd_encrypt_viewer "${CMAKE_CURRENT_SOURCE_DIR}/src/encrypt/viewer.html")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${putgd_encrypt_viewer}")
file(READ "${putgd_encrypt_viewer}" putgd_encrypt_viewer_hex HEX)
file(SIZE "${putgd_encrypt_viewer}" putgd_encrypt_viewer_size)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," putgd_encrypt_viewer_bytes "${putgd_encrypt_viewer_hex}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/encrypt/EncryptViewerBytes.cpp" CONTENT
"// Generated from src/encrypt/viewer.html by cmake/Encrypt.cmake.
namespace putgd::encrypt {
extern const unsigned char encryptViewerBytes[]={${putgd_encrypt_viewer_bytes}};
extern const unsigned long long encryptViewerSize=${putgd_encrypt_viewer_size};
}
" @ONLY)
# Qt Core only: page assembly and checking, no crypto.
add_library(putgd_encrypt_page STATIC src/encrypt/EncryptPage.cpp "${CMAKE_BINARY_DIR}/generated/encrypt/EncryptViewerBytes.cpp")
target_include_directories(putgd_encrypt_page PUBLIC src)
target_link_libraries(putgd_encrypt_page PUBLIC Qt6::Core putgd_build_options)
add_library(putgd_encrypt STATIC src/encrypt/EncryptCrypto.cpp src/encrypt/EncryptJobMain.cpp)
target_include_directories(putgd_encrypt PUBLIC src)
target_link_libraries(putgd_encrypt PUBLIC putgd_encrypt_page Qt6::Core putgd_build_options PRIVATE OpenSSL::Crypto)
