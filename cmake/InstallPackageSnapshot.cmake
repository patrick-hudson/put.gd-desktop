# Package configured source inputs with the compiled build, even if the checkout
# changes before installation. COPYONLY also makes normal builds reconfigure when
# an input changes, just like the configured desktop and launcher templates.
function(putgd_install_snapshot source destination)
  cmake_parse_arguments(PARSE_ARGV 2 snapshot "" "RENAME" "")
  get_filename_component(name "${source}" NAME)
  if(snapshot_RENAME)
    set(name "${snapshot_RENAME}")
  endif()
  set(copy "${CMAKE_CURRENT_BINARY_DIR}/package-inputs/${destination}/${name}")
  configure_file("${source}" "${copy}" COPYONLY)
  install(FILES "${copy}" DESTINATION "${destination}")
endfunction()
