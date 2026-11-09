# Custom HTTP uploader tests (#1361): the .sxcu and placeholder table, and the
# production provider and worker against the scripted loopback receiver.
if(BUILD_TESTING)
  add_executable(test_http_uploader tests/unit/test_http_uploader.cpp)
  target_link_libraries(test_http_uploader PRIVATE putgd_http_rules putgd_core Qt6::Test)
  add_test(NAME http_uploader_rules COMMAND test_http_uploader)
endif()
