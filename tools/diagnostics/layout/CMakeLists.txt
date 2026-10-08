cmake_minimum_required(VERSION 3.24)
project(putgd_layout_inspection LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4.2 REQUIRED COMPONENTS Core Gui Sql)
foreach(module Core Gui Sql)
    get_target_property(qt_library_type Qt6::${module} TYPE)
    if(NOT qt_library_type STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR "Use the existing dynamically linked distribution Qt libraries")
    endif()
endforeach()

add_executable(putgd-layout-inspect main.cpp)
target_include_directories(putgd-layout-inspect PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src")
target_link_libraries(putgd-layout-inspect PRIVATE Qt6::Core Qt6::Gui Qt6::Sql)
target_compile_definitions(putgd-layout-inspect PRIVATE
    LAYOUT_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    LAYOUT_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    LAYOUT_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
