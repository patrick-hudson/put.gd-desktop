# libFuzzer targets for the decoders at process boundaries (docs/testing/fuzzing.md).
# Off by default; the clang-asan preset turns on PUTGD_FUZZ so a ctest replays each
# target's corpus once as a regression test. PUTGD_FUZZ_INSTRUMENT also gives every
# library the targets link coverage instrumentation, for an actual fuzzing run:
#   cmake --preset clang-asan -DPUTGD_FUZZ_INSTRUMENT=ON
if(NOT PUTGD_FUZZ OR NOT BUILD_TESTING OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "PUTGD_FUZZ needs clang: libFuzzer comes with it")
endif()
set(putgd_fuzz_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz")
# putgd_fuzz_target(name LIBS lib...): tests/fuzz/fuzz_<name>.cpp, its corpus in
# tests/fuzz/corpus/<name>, and the ctest fuzz_<name> that replays that corpus.
function(putgd_fuzz_target name)
  cmake_parse_arguments(PARSE_ARGV 1 target "" "" "LIBS;SOURCES;DEFINITIONS")
  add_executable(fuzz_${name} tests/fuzz/fuzz_${name}.cpp ${target_SOURCES})
  target_compile_definitions(fuzz_${name} PRIVATE ${target_DEFINITIONS})
  target_include_directories(fuzz_${name} PRIVATE tests tests/fuzz)
  target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(fuzz_${name} PRIVATE putgd_build_options Qt6::Core ${target_LIBS})
  file(GLOB corpus CONFIGURE_DEPENDS "${putgd_fuzz_dir}/corpus/${name}/*")
  if(corpus)
    add_test(NAME fuzz_${name} COMMAND fuzz_${name} -runs=0 -timeout=5 -rss_limit_mb=2048 "${putgd_fuzz_dir}/corpus/${name}")
    set_tests_properties(fuzz_${name} PROPERTIES TIMEOUT 60 LABELS fuzz
      ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1;QT_LOGGING_RULES=*=false;QT_QPA_PLATFORM=offscreen")
  endif()
endfunction()

if(TARGET putgd_minisign AND OpenSSL_FOUND)
  putgd_fuzz_target(minisign LIBS putgd_minisign OpenSSL::Crypto)
endif()
putgd_fuzz_target(frames LIBS putgd_ui_control putgd_capture_control putgd_session_protocol)
putgd_fuzz_target(s3wire LIBS putgd_s3_wire)
putgd_fuzz_target(textjob LIBS putgd_text_protocol)
putgd_fuzz_target(settings LIBS putgd_core)
putgd_fuzz_target(journal LIBS putgd_core)
putgd_fuzz_target(gnome LIBS putgd_core SOURCES src/platform/gnome/GnomeProtocol.cpp DEFINITIONS PUTGD_GNOME_EXTENSION_VERSION=1)
# Compiled in directly: each needs only Qt Core, not the libraries around it.
putgd_fuzz_target(localipc LIBS putgd_log SOURCES src/app/LocalIpc.cpp)
putgd_fuzz_target(hyprland LIBS putgd_core SOURCES src/platform/hyprland/HyprlandIpc.cpp)
# The custom uploader reader and rules, and the import (HttpImport.cpp) that runs them on a file.
putgd_fuzz_target(httpimport LIBS putgd_http_rules putgd_core putgd_credentials SOURCES src/storage/http/HttpImport.cpp)
# The SFTP v3 replies putgd-sftp-worker reads from a server (#1871).
putgd_fuzz_target(sftpwire LIBS putgd_sftp_wire)
# Quick redact's matching over recognized words, screen text from any page (#1897).
putgd_fuzz_target(quickredact LIBS putgd_editor)
