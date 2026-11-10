# Scroll capture (#1376): the stitcher and the control loop on a synthetic
# application. No display, no helper; a wrong stitch is a valid-looking image,
# so every case compares pixels.
if(BUILD_TESTING)
  add_executable(test_scroll_stitcher tests/unit/test_scroll_stitcher.cpp tests/fixtures/ScrollSynthetic.cpp
    src/core/ScrollStitcher.cpp src/record/ScrollRun.cpp)
  target_include_directories(test_scroll_stitcher PRIVATE src)
  target_link_libraries(test_scroll_stitcher PRIVATE putgd_build_options)
  # Row hashing over thousands of frames: optimized even in a Debug tree, or the suite takes a minute.
  target_compile_options(test_scroll_stitcher PRIVATE -O2)
  add_test(NAME scroll_stitcher_contract COMMAND test_scroll_stitcher)
  set_tests_properties(scroll_stitcher_contract PROPERTIES TIMEOUT 120)
endif()
