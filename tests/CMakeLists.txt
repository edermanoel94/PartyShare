include(GoogleTest)

# The client core is not built when DV_BUILD_CLIENT is off, and the tests that
# drive it cannot be either. Keeping the two lists apart is what makes
# -DDV_BUILD_CLIENT=OFF, which docs/build.md offers to machines without Qt,
# configure instead of failing on a target that was never defined.
set(dv_unit_sources
  unit/test_result.cpp
  unit/test_logging.cpp
  unit/test_config.cpp
  unit/test_crash_reporter.cpp
  unit/test_models.cpp
  unit/test_protocol.cpp
  unit/test_room_manager.cpp
  unit/test_authenticator.cpp
  unit/test_hub.cpp
  unit/test_permissions.cpp
  unit/test_hub_admin.cpp
  unit/test_user_store.cpp
  unit/test_chat_store.cpp
  unit/test_video_feedback.cpp
  unit/test_bandwidth_estimator.cpp
)

set(dv_integration_sources
  integration/test_signaling_server.cpp
  integration/test_admin_flow.cpp
)

set(dv_client_test_libraries)

if(TARGET dv::client_core)
  list(APPEND dv_unit_sources
    unit/test_video_pipeline.cpp
    unit/test_reconnect_delay.cpp
    unit/test_network_quality.cpp
    unit/test_network_impairment.cpp
  )
  list(APPEND dv_integration_sources
    integration/test_signaling_client.cpp
    integration/test_sfu.cpp
    integration/test_call_session.cpp
  )
  list(APPEND dv_client_test_libraries dv::client_core)
endif()

add_executable(dv_unit_tests ${dv_unit_sources})

# gmock is deliberately absent, and it costs one assertion in
# unit/test_video_feedback.cpp that sorts instead of using a matcher.
#
# On Windows, vcpkg's dynamic triplet builds gmock.dll and gmock_main.dll with
# their own copy of GoogleTest compiled in: neither of them links gtest.dll, and
# gmock.dll exports MakeAndRegisterTestInfo itself. gtest_main.dll does link
# gtest.dll. So an executable that links both binds its TEST() registrations to
# the registry inside gmock.dll while main() reads the one inside gtest.dll, and
# the two never meet.
#
# The result is not a link error. It is an executable that starts, prints "this
# test program does NOT link in any test case", and exits 0. The first Windows
# build of this repository ran 63 tests instead of 277 and reported success,
# because ctest only treats *no tests at all* as a failure and the integration
# binary, which never linked gmock, kept the count above zero.
target_link_libraries(dv_unit_tests PRIVATE
  dv::shared
  dv::server_core
  ${dv_client_test_libraries}
  GTest::gtest
  GTest::gtest_main
)

dv_set_target_warnings(dv_unit_tests)
dv_enable_sanitizers(dv_unit_tests)

gtest_discover_tests(dv_unit_tests
  PROPERTIES LABELS unit ${DV_SANITIZER_TEST_PROPERTIES}
  DISCOVERY_TIMEOUT 30
)

# Integration tests bind real sockets, so they are labelled separately from the
# unit tests and can be excluded with `ctest -LE integration`.
add_executable(dv_integration_tests ${dv_integration_sources})

target_include_directories(dv_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/integration)

target_link_libraries(dv_integration_tests PRIVATE
  ${dv_client_test_libraries}
  dv::server_core
  GTest::gtest
  GTest::gtest_main
)

dv_set_target_warnings(dv_integration_tests)
dv_enable_sanitizers(dv_integration_tests)

gtest_discover_tests(dv_integration_tests
  PROPERTIES LABELS integration TIMEOUT 60 ${DV_SANITIZER_TEST_PROPERTIES}
  DISCOVERY_TIMEOUT 30
)

# The MongoDB tests need the driver at build time and a database at run time.
# The driver is the compile time half, and the database is the run time one:
# without DV_TEST_MONGO_URI each test skips itself rather than failing, so a
# developer with no MongoDB still runs the whole suite. Their own label, so
# `ctest -L mongo` is the way to ask for them and `ctest -LE mongo` the way to
# leave them out.
if(DV_ENABLE_MONGO)
  add_executable(dv_mongo_tests integration/test_mongo_store.cpp)

  target_link_libraries(dv_mongo_tests PRIVATE
    dv::server_core
    GTest::gtest
    GTest::gtest_main
  )

  dv_set_target_warnings(dv_mongo_tests)
  dv_enable_sanitizers(dv_mongo_tests)

  gtest_discover_tests(dv_mongo_tests
    PROPERTIES LABELS mongo TIMEOUT 60
    DISCOVERY_TIMEOUT 30
  )
endif()

# The media tests need libwebrtc, so they are a target of their own, built only
# when the client media layer is. They are slower than the rest, because ICE and
# DTLS take real time, and they are labelled separately for that reason.
if(DV_BUILD_CLIENT_MEDIA)
  add_executable(dv_media_tests
    integration/test_media_end_to_end.cpp
    integration/test_network_impairment.cpp
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
