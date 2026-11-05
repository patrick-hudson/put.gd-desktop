cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(putgdIconAssets LANGUAGES CXX)
  include(GNUInstallDirs)
  include(CTest)
endif()
find_package(Qt6 6.4.2 REQUIRED COMPONENTS Core Gui Svg)

# This build tool is never installed. It also works without a display server.
add_executable(putgd-render-icons EXCLUDE_FROM_ALL render.cpp)
target_compile_features(putgd-render-icons PRIVATE cxx_std_20)
target_link_libraries(putgd-render-icons PRIVATE Qt6::Core Qt6::Gui Qt6::Svg)
configure_file(../putgd.svg putgd.svg COPYONLY)
set(icon_outputs)
foreach(size 16 22 24 32 48 64 128 256)
  set(icon "${CMAKE_CURRENT_BINARY_DIR}/${size}x${size}/apps/putgd.png")
  list(APPEND icon_outputs "${icon}")
  install(FILES "${icon}" DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps")
endforeach()
add_custom_command(OUTPUT ${icon_outputs}
  COMMAND putgd-render-icons "${CMAKE_CURRENT_BINARY_DIR}/putgd.svg" "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS putgd-render-icons "${CMAKE_CURRENT_BINARY_DIR}/putgd.svg"
  VERBATIM)
add_custom_target(putgd-icons ALL DEPENDS ${icon_outputs})
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME desktop_icon_assets
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_render.py" "$<TARGET_FILE:putgd-render-icons>")
endif()
