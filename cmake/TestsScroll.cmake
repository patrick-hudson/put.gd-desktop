# Scroll capture (#1376): the stitcher and the control loop on a synthetic
# application. No display, no helper: only what an image reported as whole
# while rows are missing, the output bound and Stop would hide.
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

# The real helper session on a private Xvfb display against the fixture
# window: what the helper injects, and when, as the X server's raw input shows
# it. Not desktop proof (no compositor, no window manager).
if(BUILD_TESTING AND TARGET putgd-record)
  add_executable(test_scroll_page_fixture tests/fixtures/scroll_page_fixture.cpp)
  target_link_libraries(test_scroll_page_fixture PRIVATE putgd_build_options Qt6::Widgets)
  add_executable(test_scroll_capture_x11 tests/integration/test_scroll_capture_x11.cpp)
  target_link_libraries(test_scroll_capture_x11 PRIVATE putgd_app putgd_ui_control putgd_build_identity Qt6::Gui Qt6::Test PkgConfig::PUTGD_XCB PkgConfig::PUTGD_XCB_XINPUT)
  target_compile_definitions(test_scroll_capture_x11 PRIVATE PUTGD_RECORD_HELPER="$<TARGET_FILE:putgd-record>" PUTGD_SCROLL_FIXTURE="$<TARGET_FILE:test_scroll_page_fixture>")
  add_dependencies(test_scroll_capture_x11 putgd-record test_scroll_page_fixture)
  # A lab driver: one capture through the production backend and helper, without a resident.
  add_executable(test_scroll_capture_driver tests/fixtures/scroll_capture_driver.cpp)
  target_link_libraries(test_scroll_capture_driver PRIVATE putgd_app putgd_ui_control Qt6::Gui)
  add_dependencies(test_scroll_capture_driver putgd-record)
  putgd_xvfb_display(xvfb)
  add_test(NAME scroll_capture_x11 COMMAND ${PUTGD_XVFB_RUN} ${xvfb} -s "-screen 0 1280x900x24 -noreset" $<TARGET_FILE:test_scroll_capture_x11>)
  set_tests_properties(scroll_capture_x11 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb;XDG_SESSION_TYPE=x11;WAYLAND_DISPLAY=" TIMEOUT 300)
endif()
