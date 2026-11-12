# The UI catalog, the interface benchmarks and the GUI tests.
# Included inside if(BUILD_TESTING) in the top-level CMakeLists.txt.
  # A fixed build identity (tests/gui/catalog_build_identity.cpp) keeps the version in its renders stable.
  add_executable(putgd-ui-catalog tests/gui/ui_catalog.cpp tests/gui/catalog_build_identity.cpp)
  target_include_directories(putgd-ui-catalog PRIVATE tests src)
  target_link_libraries(putgd-ui-catalog PRIVATE putgd_ui_application putgd_editor_window putgd_tray putgd_app putgd_s3 Qt6::Test)
  # The manifest still names the real source build (the surface driver's live
  # gate binds a catalog to one exact commit): read at run time, so renders
  # keep the fixed identity above.
  target_compile_definitions(putgd-ui-catalog PRIVATE PUTGD_SOURCE_BUILD_INFO="${PUTGD_BUILD_IDENTITY_DIR}/build-info.json")
  add_dependencies(putgd-ui-catalog putgd_refresh_build_identity)
  if(TARGET putgd-record)
    # The review window and control bar states (synthetic UI catalog states).
    target_sources(putgd-ui-catalog PRIVATE src/record/ScrollBar.cpp src/ui/RegionOverlay.cpp src/ui/RegionSelector.h src/record/ReviewWindow.cpp src/record/Overlays.cpp src/record/BarPlacement.cpp src/record/ClipExporter.cpp src/record/AvSync.cpp src/record/DebugLog.cpp src/record/record.qrc)
    target_link_libraries(putgd-ui-catalog PRIVATE putgd_fonts putgd_file_dialog Qt6::DBus PkgConfig::PUTGD_AV PkgConfig::PUTGD_XCB)
    target_compile_definitions(putgd-ui-catalog PRIVATE PUTGD_CATALOG_REVIEW=1)
  endif()
  # One complete render feeds the surface map and exact-build nightly admission.
  # Keep this on the Ubuntu 26.04 CI leg; other legs run the GUI tests below.
  option(PUTGD_UI_CATALOG_RENDER "Render the complete UI catalog (Ubuntu 26.04 CI leg)" OFF)
  if(PUTGD_UI_CATALOG_RENDER)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ui_catalog_render COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_catalog_render.py"
      --catalog "$<TARGET_FILE:putgd-ui-catalog>" --scheme dark
      --output "${CMAKE_CURRENT_BINARY_DIR}/ui-catalog/dark")
    set_tests_properties(ui_catalog_render PROPERTIES TIMEOUT 400)
  endif()
  # #237: theme switch timing with History (gallery) and Settings open.
  add_executable(putgd-bench-theme-switch tests/performance/bench_theme_switch.cpp)
  target_include_directories(putgd-bench-theme-switch PRIVATE tests src)
  target_link_libraries(putgd-bench-theme-switch PRIVATE putgd_ui_application putgd_app Qt6::Test)
  # History gallery responsiveness with an established synthetic history.
  add_executable(putgd-bench-history-gallery tests/performance/bench_history_gallery.cpp)
  target_include_directories(putgd-bench-history-gallery PRIVATE tests src)
  target_link_libraries(putgd-bench-history-gallery PRIVATE putgd_ui_application putgd_app Qt6::Test)
  # #1064: History list PageDown response over a 10,000-capture history.
  add_executable(putgd-bench-history-scroll tests/performance/bench_history_scroll.cpp)
  target_include_directories(putgd-bench-history-scroll PRIVATE tests src)
  target_link_libraries(putgd-bench-history-scroll PRIVATE putgd_ui_application putgd_app Qt6::Test)
  add_executable(test_editor_window tests/gui/test_editor_window.cpp)
  target_link_libraries(test_editor_window PRIVATE putgd_editor_window Qt6::Test)
  add_test(NAME gui_editor_window COMMAND test_editor_window)
  set_tests_properties(gui_editor_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_parent_death tests/integration/test_parent_death.cpp)
    target_include_directories(test_parent_death PRIVATE src)
    target_link_libraries(test_parent_death PRIVATE putgd_build_options Qt6::Core Qt6::Test)
    add_test(NAME ui_child_parent_death COMMAND test_parent_death)
    set_tests_properties(ui_child_parent_death PROPERTIES TIMEOUT 10)
  endif()
