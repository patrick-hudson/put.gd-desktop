# The opt-in benchmark driver and putgd_xvfb_display, which every Xvfb test calls.
option(PUTGD_PHYSICAL_BENCHMARKS "Build opt-in physical desktop benchmark drivers" OFF)
if(PUTGD_PHYSICAL_BENCHMARKS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(PUTGD_BENCH_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)
  add_executable(putgd-bench-capture tests/performance/bench_capture.cpp)
  target_link_libraries(putgd-bench-capture PRIVATE putgd_build_options Qt6::Widgets PkgConfig::PUTGD_XCB PkgConfig::PUTGD_BENCH_XTEST)
endif()

# Concurrent `xvfb-run -a` calls that start from the same display wait two
# seconds per attempt on the X server's lock. Each Xvfb test starts its search
# at its own display; -a still skips displays that other processes hold.
set_property(GLOBAL PROPERTY PUTGD_XVFB_DISPLAY 200)
function(putgd_xvfb_display out)
  get_property(display GLOBAL PROPERTY PUTGD_XVFB_DISPLAY)
  math(EXPR next "${display} + 1")
  set_property(GLOBAL PROPERTY PUTGD_XVFB_DISPLAY ${next})
  set(${out} -n ${display} -a PARENT_SCOPE)
endfunction()
