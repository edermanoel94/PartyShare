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
  unit/test_video_pipeline.cpp
  unit/test_reconnect_delay.cpp
  unit/test_network_quality.cpp
)

target_link_libraries(dv_unit_tests PRIVATE
  dv::shared
  dv::server_core
  dv::client_core
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
  integration/test_call_session.cpp
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

# The media tests need libwebrtc, so they are a target of their own, built only
# when the client media layer is. They are slower than the rest, because ICE and
# DTLS take real time, and they are labelled separately for that reason.
if(DV_BUILD_CLIENT_MEDIA)
  add_executable(dv_media_tests
    integration/test_media_end_to_end.cpp
    integration/test_screen_capture.cpp
  )

  target_link_libraries(dv_media_tests PRIVATE
    dv::client_core
    dv::server_core
    GTest::gtest
    GTest::gtest_main
  )

  dv_enable_sanitizers(dv_media_tests)

  gtest_discover_tests(dv_media_tests
    PROPERTIES LABELS media TIMEOUT 180
    DISCOVERY_TIMEOUT 60
  )

  # The benchmarks of section 22 of SPEC.md. A target of their own because they
  # hold a call of five for half a minute, which nobody wants in the suite that
  # runs on every change.
  add_executable(dv_benchmarks integration/test_benchmark.cpp)

  target_include_directories(dv_benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/integration)

  target_link_libraries(dv_benchmarks PRIVATE
    dv::client_core
    dv::server_core
    GTest::gtest
    GTest::gtest_main
  )

  gtest_discover_tests(dv_benchmarks
    PROPERTIES LABELS benchmark TIMEOUT 600
    DISCOVERY_TIMEOUT 60
  )
endif()
