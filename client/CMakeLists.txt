# The client is split in two on purpose.
#
# dv_client_core holds everything that is not the interface: signaling, media
# and, from M6, screen capture. It must never include a Qt header, which is the
# layering rule of section 15 of SPEC.md. Keeping it a library of its own is
# also what lets the tests drive it without starting a GUI.
#
# dv_client is the Qt executable on top of it.

# The media layer is optional at build time. It needs the libwebrtc tree built
# by scripts/build_webrtc.sh, which is 66 MB of static library that cannot be
# downloaded ready made: the published binaries carry Chromium's libc++ and do
# not link with Qt. See docs/webrtc-toolchain.md.
#
# Off by default so that the server, the tests and a CI runner without the
# toolchain still build the client library. With it off, create_audio_session
# fails with media_unavailable and everything above it still works.
option(DV_BUILD_CLIENT_MEDIA "Build the client media layer on libwebrtc" OFF)

add_library(dv_client_core STATIC
  src/app/call_session.cpp
  src/app/call_session.hpp
  src/audio/audio_session.cpp
  src/audio/audio_session.hpp
  src/network/signaling_client.cpp
  src/network/signaling_client.hpp
)
add_library(dv::client_core ALIAS dv_client_core)

target_include_directories(dv_client_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(dv_client_core PUBLIC
  dv::shared
  LibDataChannel::LibDataChannel
)

dv_set_target_warnings(dv_client_core)
dv_enable_sanitizers(dv_client_core)

if(DV_BUILD_CLIENT_MEDIA)
  find_package(libwebrtc REQUIRED)

  # A target of its own because the strict warning set cannot be applied to a
  # translation unit that includes libwebrtc headers.
  add_library(dv_client_media STATIC src/webrtc/libwebrtc_audio_session.cpp)

  target_include_directories(dv_client_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(dv_client_media PUBLIC dv::shared libwebrtc::libwebrtc)
  dv_enable_sanitizers(dv_client_media)

  # DV_WITH_LIBWEBRTC compiles out the stub in audio/audio_session.cpp, so that
  # exactly one definition of create_audio_session exists.
  target_compile_definitions(dv_client_core PRIVATE DV_WITH_LIBWEBRTC)
  target_link_libraries(dv_client_core PUBLIC dv_client_media)

  message(STATUS "Client media: libwebrtc")
else()
  message(STATUS "Client media: disabled, the client will not send or receive audio")
endif()

find_package(Qt6 6.5 QUIET COMPONENTS Widgets)

if(NOT Qt6_FOUND)
  message(FATAL_ERROR
    "Qt 6.5 or newer is required to build the client.\n"
    "Install it and point CMAKE_PREFIX_PATH at it, or configure with "
    "-DDV_BUILD_CLIENT=OFF to build only the server and the tests.")
endif()

qt_standard_project_setup()

qt_add_executable(dv_client
  src/main.cpp
  src/ui/main_window.cpp
  src/ui/main_window.hpp
)

target_link_libraries(dv_client PRIVATE
  dv::client_core
  Qt6::Widgets
)

set_target_properties(dv_client PROPERTIES
  OUTPUT_NAME "desktop-voice"
  WIN32_EXECUTABLE ON
  MACOSX_BUNDLE ON
  MACOSX_BUNDLE_BUNDLE_NAME "Desktop Voice"
  MACOSX_BUNDLE_GUI_IDENTIFIER "dev.desktopvoice.client"
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)

dv_set_target_warnings(dv_client)
dv_enable_sanitizers(dv_client)
