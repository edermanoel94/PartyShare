include(GoogleTest)

# integration/media_test_client.hpp holds a std::atomic<media::AudioStats>, and
# that struct is wider than a machine word, so the atomic is not lock free.
# GCC then emits calls to __atomic_load and __atomic_store instead of
# instructions, and those live in libatomic, which it does not link by default:
# every target that instantiates one fails at link with an undefined reference.
#
# Detected by compiling rather than by looking for the file. find_library does
# not answer this: GCC keeps libatomic.so in its own directory, which is not on
# any search path CMake walks, and the driver resolves -latomic from there
# without help. Asking the compiler whether it needs the flag is the only
# question that has the same meaning on every toolchain - MSVC never emits
# those calls, and clang resolves them out of its own runtime.
include(CheckCXXSourceCompiles)

set(dv_atomic_probe "
#include <atomic>
struct Wide { double a, b, c, d, e, f, g, h; };
int main() {
  std::atomic<Wide> value{};
  const Wide read = value.load();
  value.store(read);
  return 0;
}
")

check_cxx_source_compiles("${dv_atomic_probe}" DV_ATOMICS_NEED_NO_LIBRARY)

set(DV_LIBATOMIC "")
if(NOT DV_ATOMICS_NEED_NO_LIBRARY)
  set(CMAKE_REQUIRED_LIBRARIES atomic)
  check_cxx_source_compiles("${dv_atomic_probe}" DV_ATOMICS_NEED_LIBATOMIC)
  unset(CMAKE_REQUIRED_LIBRARIES)

  if(DV_ATOMICS_NEED_LIBATOMIC)
    set(DV_LIBATOMIC atomic)
    message(STATUS "Tests: linking libatomic for atomics wider than a word")
  else()
    # Neither works, so linking will fail with a message about __atomic_load
    # that says nothing about why. Saying it here costs one line and saves the
    # next person the search.
    message(WARNING
      "std::atomic of a wide struct neither compiles alone nor with libatomic; "
      "the test targets will fail to link")
  endif()
endif()

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
    unit/test_smoothing.cpp
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
  ${DV_LIBATOMIC}
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
  ${DV_LIBATOMIC}
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
    ${DV_LIBATOMIC}
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
    integration/test_hardware_encoder.cpp
    integration/test_media_end_to_end.cpp
    integration/test_network_impairment.cpp
    integration/test_screen_capture.cpp
  )

  target_link_libraries(dv_media_tests PRIVATE
    dv::client_core
    dv::server_core
    GTest::gtest
    GTest::gtest_main
    ${DV_LIBATOMIC}
  )

  dv_enable_sanitizers(dv_media_tests)

  gtest_discover_tests(dv_media_tests
    PROPERTIES LABELS media TIMEOUT 180
    DISCOVERY_TIMEOUT 60
  )

  # The Media Foundation backend, named explicitly.
  #
  # Without this it is never reached on a machine that has an NVIDIA card: the
  # chain probes NVENC first and NVENC wins, so the discovered run above only
  # ever covers one of the two backends. This runs the same test again with the
  # other one selected, which is the only way both are covered on one machine.
  #
  # It skips itself where there is no Media Foundation encoder, the same way
  # the discovered one skips where there is no hardware at all.
  if(WIN32)
    add_test(NAME HardwareEncoderTest.MediaFoundationEncodesH264
      COMMAND dv_media_tests --gtest_filter=HardwareEncoderTest.EncodesH264OnThisMachine)
    set_tests_properties(HardwareEncoderTest.MediaFoundationEncodesH264 PROPERTIES
      ENVIRONMENT "DV_HARDWARE_ENCODER=mediafoundation"
      LABELS media
      TIMEOUT 180
    )
  endif()

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
    ${DV_LIBATOMIC}
  )

  gtest_discover_tests(dv_benchmarks
    PROPERTIES LABELS benchmark TIMEOUT 600
    DISCOVERY_TIMEOUT 60
  )
endif()
