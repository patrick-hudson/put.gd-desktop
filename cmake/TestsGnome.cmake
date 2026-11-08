# GNOME bridge, extension and KWin transfer tests.
# Included inside if(BUILD_TESTING) in the top-level CMakeLists.txt.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_gnome_bridge tests/integration/test_gnome_bridge.cpp)
    target_link_libraries(test_gnome_bridge PRIVATE putgd_gnome Qt6::Test)
    add_library(putgd_gnome_mime_fixture MODULE tests/fixtures/GnomeImageMimePlugin.cpp)
    target_link_libraries(putgd_gnome_mime_fixture PRIVATE Qt6::Gui)
    set_target_properties(putgd_gnome_mime_fixture PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture/imageformats")
    target_compile_definitions(test_gnome_bridge PRIVATE
      PUTGD_GNOME_MIME_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture")
    add_dependencies(test_gnome_bridge putgd-ui putgd_gnome_mime_fixture)
    # GNOME transfers decode PNG in the Core resident without image plugins.
    add_executable(test_gnome_image_decode tests/integration/test_gnome_image_decode.cpp)
    target_link_libraries(test_gnome_image_decode PRIVATE putgd_gnome Qt6::Test)
    target_compile_definitions(test_gnome_image_decode PRIVATE
      PUTGD_IMAGE_PLUGIN_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/gnome-mime-fixture")
    add_dependencies(test_gnome_image_decode putgd-ui putgd_gnome_mime_fixture)
    add_test(NAME gnome_image_decode COMMAND test_gnome_image_decode)
    set_tests_properties(gnome_image_decode PROPERTIES TIMEOUT 60)
    add_test(NAME gnome_bridge_contract COMMAND test_gnome_bridge)
    add_executable(test_gnome_extension_setup tests/integration/test_gnome_extension_setup.cpp)
    target_link_libraries(test_gnome_extension_setup PRIVATE putgd_gnome Qt6::Test)
    add_test(NAME gnome_extension_setup COMMAND test_gnome_extension_setup)
    set_tests_properties(gnome_extension_setup PROPERTIES TIMEOUT 60)
    find_program(PUTGD_DBUS_RUN_SESSION NAMES dbus-run-session REQUIRED)
    add_test(NAME gnome_core_transport_integration COMMAND "${PUTGD_DBUS_RUN_SESSION}" -- $<TARGET_FILE:test_gnome_bridge> --core-adapter-transport)
    set_tests_properties(gnome_core_transport_integration PROPERTIES
      ENVIRONMENT "XDG_CURRENT_DESKTOP=GNOME;XDG_SESSION_TYPE=wayland;WAYLAND_DISPLAY=putgd-synthetic-wayland;DISPLAY=;QT_QPA_PLATFORM=wayland" TIMEOUT 60)
    # The extension is GJS code: use GNOME's existing runtime on its desktop,
    # never introduce Node.js as a host or guest test dependency.
    find_program(PUTGD_GJS gjs)
    if(PUTGD_GJS)
      add_test(NAME gnome_region_selection_contract COMMAND "${PUTGD_GJS}" -m "${PROJECT_SOURCE_DIR}/tests/desktop/test_gnome_region.mjs" "${PROJECT_SOURCE_DIR}/integrations/gnome/extension.js")
      set_tests_properties(gnome_region_selection_contract PROPERTIES TIMEOUT 30)
      add_test(NAME gnome_ui_ownership_contract COMMAND "${PUTGD_GJS}" -m "${PROJECT_SOURCE_DIR}/tests/desktop/test_gnome_ownership.mjs" "${PROJECT_SOURCE_DIR}/integrations/gnome/extension.js")
      set_tests_properties(gnome_ui_ownership_contract PROPERTIES TIMEOUT 30)
    else()
      message(STATUS "GNOME extension ownership contract runs in the GNOME lab with its native GJS runtime")
    endif()
    add_executable(test_kwin_transfer tests/integration/test_kwin_transfer.cpp)
    target_link_libraries(test_kwin_transfer PRIVATE putgd_kde Qt6::Test)
    add_test(NAME kwin_transfer_contract COMMAND test_kwin_transfer)
  endif()
