# Scroll capture (#1515): the session loop on a synthetic application. No
# display, no helper: only what a wrong image, a capture that never ends, an
# injection while the user has the mouse and Done or Cancel would hide.
if(BUILD_TESTING)
  add_executable(test_scroll_session tests/unit/test_scroll_session.cpp tests/fixtures/ScrollSynthetic.cpp
    src/core/ScrollTracker.cpp src/record/ScrollSession.cpp src/record/ScrollSpool.cpp)
  target_include_directories(test_scroll_session PRIVATE src)
  target_link_libraries(test_scroll_session PRIVATE putgd_build_options)
  # The tracker over thousands of frames: optimized even in a Debug tree, or the suite takes minutes.
  target_compile_options(test_scroll_session PRIVATE -O2)
  add_test(NAME scroll_session_contract COMMAND test_scroll_session)
  set_tests_properties(scroll_session_contract PROPERTIES TIMEOUT 180)
  # The tracker (#1515) on synthetic scenes: a wrong offset, a pinned part
  # repeated, a row lost at a seam.
  add_executable(test_scroll_tracker tests/unit/test_scroll_tracker.cpp tests/fixtures/ScrollSynthetic.cpp
    src/core/ScrollTracker.cpp)
  target_include_directories(test_scroll_tracker PRIVATE src)
  target_link_libraries(test_scroll_tracker PRIVATE putgd_build_options)
  target_compile_options(test_scroll_tracker PRIVATE -O2)
  add_test(NAME scroll_tracker_contract COMMAND test_scroll_tracker)
  set_tests_properties(scroll_tracker_contract PROPERTIES TIMEOUT 120)
  # A lab tool: replays recorded frames through the tracker
  # (tools/lab/scroll_tracker_score.py). Not a test by itself.
  add_executable(test_scroll_tracker_replay tests/fixtures/scroll_tracker_replay.cpp src/core/ScrollTracker.cpp)
  target_include_directories(test_scroll_tracker_replay PRIVATE src)
  target_link_libraries(test_scroll_tracker_replay PRIVATE putgd_build_options Qt6::Gui)
  target_compile_options(test_scroll_tracker_replay PRIVATE -O2)
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
