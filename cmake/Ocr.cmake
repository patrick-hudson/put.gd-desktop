# The putgd-ocr text recognition helper.
# Text recognition and QR codes (#1428): Tesseract and ZXing-cpp live in this
# temporary process only; the resident, putgd-ui, putgd-record and the workers
# never link or load them. AUTO builds the helper when both libraries are
# present, as a developer build does for PUTGD_RECORDING; every package build
# sets ON, and a package build without the libraries fails.
set(PUTGD_OCR "AUTO" CACHE STRING "Build the putgd-ocr text recognition helper: AUTO (when Tesseract and ZXing-cpp are present), ON (required) or OFF")
set_property(CACHE PUTGD_OCR PROPERTY STRINGS AUTO ON OFF)
if(NOT PUTGD_OCR MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "PUTGD_OCR must be AUTO, ON or OFF")
endif()
if(PUTGD_OCR STREQUAL "OFF")
  return()
endif()
set(putgd_ocr_missing "")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  # The seccomp filter is written for x86_64, the only packaged architecture.
  set(putgd_ocr_missing "Linux on x86_64")
else()
  find_package(PkgConfig REQUIRED)
  # Tesseract 5.3.4 (Ubuntu 24.04, Mint 22) or later; its .pc requires lept.
  pkg_check_modules(PUTGD_TESSERACT IMPORTED_TARGET tesseract>=5.3.4)
  # ZXing-cpp 2.2.1 (Ubuntu 24.04, Mint 22, Fedora), 2.3.0 (Debian 13, Ubuntu 26.04), 3.1.1 (Arch).
  pkg_check_modules(PUTGD_ZXING IMPORTED_TARGET zxing>=2.2.1)
  if(NOT PUTGD_TESSERACT_FOUND)
    list(APPEND putgd_ocr_missing "tesseract>=5.3.4 and lept (libtesseract-dev, libleptonica-dev)")
  endif()
  if(NOT PUTGD_ZXING_FOUND)
    list(APPEND putgd_ocr_missing "zxing>=2.2.1 (libzxing-dev)")
  endif()
endif()
if(putgd_ocr_missing)
  if(PUTGD_OCR STREQUAL "ON")
    message(FATAL_ERROR "PUTGD_OCR=ON needs ${putgd_ocr_missing}")
  endif()
  message(STATUS "putgd-ocr is not built: it needs ${putgd_ocr_missing}")
  return()
endif()

# The engine-free parts: sanitizing, scale and bands, words, the result JSON.
add_library(putgd_ocr_logic STATIC src/ocr/Sanitize.cpp src/ocr/Layout.cpp src/ocr/Words.cpp src/ocr/Result.cpp)
target_include_directories(putgd_ocr_logic PUBLIC src)
target_link_libraries(putgd_ocr_logic PUBLIC Qt6::Core putgd_build_options)
# The engines, confinement and the memfd transfer.
add_library(putgd_ocr_engine STATIC src/ocr/TextEngine.cpp src/ocr/CodeReader.cpp src/ocr/Recognize.cpp src/ocr/Confinement.cpp src/ocr/Descriptors.cpp)
target_link_libraries(putgd_ocr_engine PUBLIC putgd_ocr_logic PRIVATE PkgConfig::PUTGD_TESSERACT PkgConfig::PUTGD_ZXING)
add_executable(putgd-ocr src/ocr/main.cpp)
target_link_libraries(putgd-ocr PRIVATE putgd_ocr_engine putgd_ui_control putgd_build_identity Qt6::Core)
# Some distributions' tesseract.pc lists libcurl and libarchive among its own
# Libs; the helper needs neither directly.
target_link_options(putgd-ocr PRIVATE "LINKER:--as-needed")
install(TARGETS putgd-ocr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
  # The character filter, blanked code areas, band seams and the input limit.
  add_executable(test_ocr_logic tests/unit/test_ocr_logic.cpp)
  target_link_libraries(test_ocr_logic PRIVATE putgd_ocr_logic Qt6::Test)
  add_test(NAME ocr_logic_contract COMMAND test_ocr_logic)
  # A parent for the real helper: sealed memfd in, memfd out, the control
  # protocol, stage timings and the child's peak RSS. Also a command-line
  # driver for measurements (test_ocr_run IMAGE.png).
  add_library(test_ocr_parent STATIC tests/integration/ocr/OcrParent.cpp)
  target_include_directories(test_ocr_parent PUBLIC src tests)
  target_link_libraries(test_ocr_parent PUBLIC putgd_ui_control putgd_build_identity Qt6::Gui)
  add_executable(test_ocr_run tests/integration/ocr/ocr_run.cpp)
  target_link_libraries(test_ocr_run PRIVATE test_ocr_parent)
  add_executable(test_ocr_helper tests/integration/test_ocr_helper.cpp)
  target_link_libraries(test_ocr_helper PRIVATE test_ocr_parent Qt6::Test)
  target_compile_definitions(test_ocr_helper PRIVATE "PUTGD_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
  add_dependencies(test_ocr_helper putgd-ocr)
  add_test(NAME ocr_helper_integration COMMAND test_ocr_helper)
  set_tests_properties(ocr_helper_integration PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PUTGD_OCR_HELPER=$<TARGET_FILE:putgd-ocr>")
  # From inside a test build of the helper, each forbidden thing fails.
  add_executable(test_ocr_confinement_probe tests/integration/ocr/confinement_probe.cpp)
  target_link_libraries(test_ocr_confinement_probe PRIVATE putgd_ocr_engine)
  add_executable(test_ocr_confinement tests/integration/test_ocr_confinement.cpp)
  target_link_libraries(test_ocr_confinement PRIVATE Qt6::Core Qt6::Test)
  add_dependencies(test_ocr_confinement test_ocr_confinement_probe)
  add_test(NAME ocr_confinement_integration COMMAND test_ocr_confinement)
  set_tests_properties(ocr_confinement_integration PROPERTIES TIMEOUT 120 ENVIRONMENT "PUTGD_OCR_PROBE=$<TARGET_FILE:test_ocr_confinement_probe>")
  # The method comparisons of decision 4, in process: bench_ocr_method --help.
  add_executable(bench_ocr_method tests/integration/ocr/ocr_method.cpp)
  target_link_libraries(bench_ocr_method PRIVATE putgd_ocr_engine Qt6::Gui)
endif()
