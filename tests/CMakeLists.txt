include(GoogleTest)

add_executable(dv_unit_tests
  unit/test_result.cpp
  unit/test_logging.cpp
  unit/test_config.cpp
  unit/test_models.cpp
  unit/test_protocol.cpp
  unit/test_room_manager.cpp
  unit/test_authenticator.cpp
  unit/test_hub.cpp
)

target_link_libraries(dv_unit_tests PRIVATE
  dv::shared
  dv::server_core
  GTest::gtest
  GTest::gtest_main
)

dv_set_target_warnings(dv_unit_tests)
dv_enable_sanitizers(dv_unit_tests)

gtest_discover_tests(dv_unit_tests
  PROPERTIES LABELS unit
  DISCOVERY_TIMEOUT 30
)

# Integration tests bind real sockets, so they are labelled separately from the
# unit tests and can be excluded with `ctest -LE integration`.
add_executable(dv_integration_tests
  integration/test_signaling_server.cpp
  integration/test_signaling_client.cpp
  integration/test_sfu.cpp
)

target_include_directories(dv_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/integration)

target_link_libraries(dv_integration_tests PRIVATE
  dv::client_core
  dv::server_core
  GTest::gtest
  GTest::gtest_main
)

dv_set_target_warnings(dv_integration_tests)
dv_enable_sanitizers(dv_integration_tests)

gtest_discover_tests(dv_integration_tests
  PROPERTIES LABELS integration TIMEOUT 60
  DISCOVERY_TIMEOUT 30
)
