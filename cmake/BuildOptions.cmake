# Build options: warnings, sanitizers, debug splitting and the crash handler.
set(PUTGD_SANITIZER "" CACHE STRING "Sanitizer: address-undefined or thread")
add_library(putgd_build_options INTERFACE)
target_compile_features(putgd_build_options INTERFACE cxx_std_20)
target_compile_options(putgd_build_options INTERFACE -Wall -Wextra -Wpedantic)
if(PUTGD_SANITIZER STREQUAL "address-undefined")
  target_compile_options(putgd_build_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(putgd_build_options INTERFACE -fsanitize=address,undefined)
elseif(PUTGD_SANITIZER STREQUAL "thread")
  target_compile_options(putgd_build_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(putgd_build_options INTERFACE -fsanitize=thread)
elseif(NOT PUTGD_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown PUTGD_SANITIZER: ${PUTGD_SANITIZER}")
endif()

find_package(Threads REQUIRED)
# Release builds keep debug information, split out of each product binary into
# <file>.debug before anything hashes or installs it (#124, docs/design/diagnostics.md).
# AUTO splits Release builds; ON also splits other build types (a development
# build with the crash trigger, to rehearse symbolization); OFF never splits.
set(PUTGD_SPLIT_DEBUG "AUTO" CACHE STRING "Split debug information into <binary>.debug: AUTO (Release), ON or OFF")
set_property(CACHE PUTGD_SPLIT_DEBUG PROPERTY STRINGS AUTO ON OFF)
set(putgd_split_debug OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (PUTGD_SPLIT_DEBUG STREQUAL "ON" OR (PUTGD_SPLIT_DEBUG STREQUAL "AUTO" AND CMAKE_BUILD_TYPE STREQUAL "Release")))
  find_program(PUTGD_OBJCOPY NAMES objcopy REQUIRED)
  target_compile_options(putgd_build_options INTERFACE -g)
  set(putgd_split_debug ON)
elseif(NOT PUTGD_SPLIT_DEBUG MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "PUTGD_SPLIT_DEBUG must be AUTO, ON or OFF")
endif()
# The built-in crash handler (#124): every product executable links this object
# library, which installs the handler before main. Sanitizer builds compile it
# disabled; the test-only crash trigger exists only in development builds.
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$" OR NOT PUTGD_RELEASE_TAG STREQUAL "")
  set(putgd_crash_trigger_default OFF)
else()
  set(putgd_crash_trigger_default ON)
endif()
option(PUTGD_CRASH_TRIGGER "Development builds: PUTGD_TEST_CRASH crashes a chosen executable on purpose" ${putgd_crash_trigger_default})
if(PUTGD_CRASH_TRIGGER AND (CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$" OR NOT PUTGD_RELEASE_TAG STREQUAL ""))
  message(FATAL_ERROR "The test-only crash trigger is never compiled into release builds")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(putgd_crash OBJECT src/core/CrashHandler.cpp)
  set_target_properties(putgd_crash PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(putgd_crash PUBLIC src)
  target_link_libraries(putgd_crash PUBLIC putgd_build_options putgd_build_identity Threads::Threads ${CMAKE_DL_LIBS})
  if(NOT PUTGD_SANITIZER STREQUAL "")
    target_compile_definitions(putgd_crash PUBLIC PUTGD_CRASH_SANITIZED=1)
  else()
    # Its pthread_create gives every thread an alternate signal stack (#156);
    # exported, it is the one Qt, libstdc++ and other libraries call too.
    target_link_options(putgd_crash INTERFACE "LINKER:--export-dynamic-symbol=pthread_create")
  endif()
  if(PUTGD_CRASH_TRIGGER)
    target_compile_definitions(putgd_crash PUBLIC PUTGD_CRASH_TRIGGER=1)
  endif()
endif()
