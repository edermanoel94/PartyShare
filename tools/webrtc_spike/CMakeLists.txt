# M3 spike. Not part of the product: its only job is to prove that a given
# libwebrtc links and runs on this platform before the project commits to it.
# Enable with -DDV_ENABLE_WEBRTC_SPIKE=ON.

find_package(libwebrtc REQUIRED)

# The sharpest test of the standard library question is simply to link
# dv::shared, which is built with the system standard library, and then pass a
# std::string across the libwebrtc boundary.
#
# On a tree built by scripts/build_webrtc.sh that has to work. On a prebuilt
# tree carrying Chromium's libc++ it cannot, so the spike is built standalone
# there and reports the limitation instead of failing to link.
if(DV_WEBRTC_USE_BUNDLED_LIBCXX AND UNIX AND NOT APPLE)
  set(_dv_spike_link_shared OFF)
else()
  set(_dv_spike_link_shared ON)
endif()

add_executable(dv_webrtc_spike main.cpp)

target_link_libraries(dv_webrtc_spike PRIVATE libwebrtc::libwebrtc)

if(_dv_spike_link_shared)
  message(STATUS "libwebrtc spike: linking dv::shared to check standard library compatibility")
  target_link_libraries(dv_webrtc_spike PRIVATE dv::shared)
  target_compile_definitions(dv_webrtc_spike PRIVATE DV_SPIKE_WITH_SHARED)
else()
  message(STATUS "libwebrtc spike: standalone, this libwebrtc carries its own libc++")
endif()

set_target_properties(dv_webrtc_spike PROPERTIES OUTPUT_NAME "webrtc-spike")

# The strict warning set is deliberately not applied: libwebrtc headers are not
# clean under it, and this target is throwaway.
dv_enable_sanitizers(dv_webrtc_spike)
