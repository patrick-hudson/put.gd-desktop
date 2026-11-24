# The file-manager menu plugins.
# File-manager menu plugins (#1105): AUTO builds each whose headers pkg-config
# finds; package builds set ON. Before packaging/linux, which installs them.
foreach(file_manager NAUTILUS CAJA PEONY)
  set(PUTGD_${file_manager}_PLUGIN AUTO CACHE STRING "Build the ${file_manager} menu plugin: AUTO, ON or OFF")
  set_property(CACHE PUTGD_${file_manager}_PLUGIN PROPERTY STRINGS AUTO ON OFF)
endforeach()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(src/filemanager)
endif()
