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
# toolchain still build the client library. With it off, create_media_session
# fails with media_unavailable and everything above it still works.
option(DV_BUILD_CLIENT_MEDIA "Build the client media layer on libwebrtc" OFF)

add_library(dv_client_core STATIC
  src/app/call_session.cpp
  src/app/call_session.hpp
  src/app/network_quality.hpp
  src/media/media_session.cpp
  src/media/media_session.hpp
  src/media/network_impairment.cpp
  src/media/network_impairment.hpp
  src/network/signaling_client.cpp
  src/network/signaling_client.hpp
  src/video/frame_queue.hpp
  src/video/frame_size.hpp
  src/video/screen_capturer.cpp
  src/video/screen_capturer.hpp
  src/video/video_frame.hpp
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
  # NVENC is the hardware encoder this project has. The plan named VAAPI for
  # Linux, which is right on Intel and AMD and is not an answer on NVIDIA,
  # whose driver does no encoding through it. Nothing is linked: the driver's
  # libraries are opened at runtime, so a binary built with this still runs on
  # a machine with no NVIDIA card.
  option(DV_HARDWARE_ENCODER_NVENC "Build the NVENC hardware encoder backend" ON)

  add_library(dv_client_media STATIC
    src/webrtc/hardware_encoder.hpp
    src/webrtc/hardware_encoder_none.cpp
    src/webrtc/hardware_encoder_nvenc.cpp
    src/webrtc/impaired_socket_factory.cpp
    src/webrtc/impaired_socket_factory.hpp
    src/webrtc/video_encoder_factory.cpp
    src/webrtc/video_encoder_factory.hpp
    src/webrtc/libwebrtc_media_session.cpp
    src/webrtc/libwebrtc_screen_capturer.cpp
  )

  target_include_directories(dv_client_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  if(DV_HARDWARE_ENCODER_NVENC AND NOT APPLE)
    target_compile_definitions(dv_client_media PRIVATE DV_WITH_NVENC)
    target_include_directories(dv_client_media PRIVATE
      ${CMAKE_SOURCE_DIR}/third_party/nvcodec)
    # Only for dlopen: the encoder itself is found at runtime.
    target_link_libraries(dv_client_media PRIVATE ${CMAKE_DL_LIBS})
    message(STATUS "Hardware encoder: NVENC")
  else()
    message(STATUS "Hardware encoder: none, the screen is encoded in software")
  endif()
  # The two libraries depend on each other: the core calls into the media layer
  # to create a session, and the media layer calls back into the core for the
  # network impairment counters. Declaring both directions is what makes CMake
  # repeat them on the link line; leaving the second one out builds and then
  # fails to link, in whichever order the linker happened to be given.
  target_link_libraries(dv_client_media PUBLIC dv::shared libwebrtc::libwebrtc)
  target_link_libraries(dv_client_media PRIVATE dv_client_core)
  dv_enable_sanitizers(dv_client_media)

  # DV_WITH_LIBWEBRTC compiles out the stub in media/media_session.cpp, so that
  # exactly one definition of create_media_session exists.
  target_compile_definitions(dv_client_core PRIVATE DV_WITH_LIBWEBRTC)
  target_link_libraries(dv_client_core PUBLIC dv_client_media)

  message(STATUS "Client media: libwebrtc")
else()
  message(STATUS "Client media: disabled, the client will not send or receive audio")
endif()

# The interface is optional separately from the rest of the client, because
# dv_client_core has no Qt in it and is worth building where Qt is not wanted.
# The sanitizer job of the CI is the case: Qt is not built with sanitizers, but
# the client core is exactly where the threading that ASan and UBSan are for
# lives, and the tests need it.
option(DV_BUILD_CLIENT_UI "Build the Qt interface on top of the client core" ON)

if(NOT DV_BUILD_CLIENT_UI)
  message(STATUS "Client interface: disabled, building the client core only")
  return()
endif()

find_package(Qt6 6.5 QUIET COMPONENTS Widgets)

if(NOT Qt6_FOUND)
  message(FATAL_ERROR
    "Qt 6.5 or newer is required to build the client interface.\n"
    "Install it and point CMAKE_PREFIX_PATH at it, or configure with "
    "-DDV_BUILD_CLIENT_UI=OFF to build the client core, the server and the "
    "tests without it.")
endif()

qt_standard_project_setup()

# The icon and the version block Explorer reads off the executable. Configured
# rather than static, so the properties dialog cannot disagree with the tag.
if(WIN32)
  configure_file(${CMAKE_SOURCE_DIR}/assets/partyshare.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/partyshare.rc @ONLY)
  set(dv_client_resources ${CMAKE_CURRENT_BINARY_DIR}/partyshare.rc)
endif()

# The macOS icon has to be a source of the target, not a file copied next to it:
# it is the bundle's Resources directory that Finder reads, and a bundle without
# one gets the blank sheet of paper that says nobody finished the packaging.
if(APPLE)
  set(dv_client_resources ${CMAKE_SOURCE_DIR}/assets/partyshare.icns)
  set_source_files_properties(${dv_client_resources} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
endif()

qt_add_executable(dv_client
  ${dv_client_resources}
  src/main.cpp
  src/ui/admin_panel.cpp
  src/ui/admin_panel.hpp
  src/ui/main_window.cpp
  src/ui/main_window.hpp
  src/ui/screen_view.cpp
  src/ui/screen_view.hpp
  src/ui/settings_dialog.cpp
  src/ui/settings_dialog.hpp
)

target_link_libraries(dv_client PRIVATE
  dv::client_core
  Qt6::Widgets
)

set_target_properties(dv_client PROPERTIES
  OUTPUT_NAME "partyshare"
  WIN32_EXECUTABLE ON
  MACOSX_BUNDLE ON
  MACOSX_BUNDLE_BUNDLE_NAME "PartyShare"
  MACOSX_BUNDLE_GUI_IDENTIFIER "dev.partyshare.client"
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_ICON_FILE "partyshare.icns"
)

dv_set_target_warnings(dv_client)
dv_enable_sanitizers(dv_client)

# Windows has no rpath and no package manager: a program finds its libraries
# beside its own executable or it does not start. CMake installs the target and
# nothing else, and windeployqt knows only about Qt, so the first staged tree
# this repository ever produced held partyshare.exe, the Qt runtime, and none of
# spdlog.dll, fmt.dll or datachannel.dll. That is an installer that puts a
# program on a machine where it cannot run, and the release job's own check did
# not notice, because it looks for the executable and the Qt platform plugin and
# both of those were there.
#
# Only Windows, because the other two already solve this: the AppImage is built
# by linuxdeploy from the installed tree, and the macOS bundle by its own
# tooling. Declaring the set on those platforms would hand CMake a second
# opinion about libraries it already handles.
set(dv_client_runtime_dependencies)
if(WIN32)
  set(dv_client_runtime_dependencies RUNTIME_DEPENDENCY_SET dv_client_runtime)
endif()

install(TARGETS dv_client
  ${dv_client_runtime_dependencies}
  BUNDLE DESTINATION .
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(WIN32)
  # CMake 4 normalises the paths this reads before matching them; older CMake
  # hands them over exactly as the loader recorded them, which on Windows means
  # a single path with both separators in it, "C:\WINDOWS\system32/shell32.dll".
  # Asking for the new behaviour is what lets the expression below stay readable
  # instead of having to match either spelling.
  if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
  endif()

  # Two directories, because the runtime arrives from two places: vcpkg's
  # z-applocal step has already put spdlog, fmt, libdatachannel and everything
  # under them next to the executable in the build tree, and Qt lives wherever
  # CMAKE_PREFIX_PATH pointed. The Qt directory is read off the imported target
  # rather than written down, so a machine with Qt somewhere else still resolves.
  #
  # Qt is included rather than left to windeployqt because a dependency that
  # cannot be resolved is an error, not a shrug: excluding Qt by name still
  # leaves it unresolved and stops the install. windeployqt runs afterwards
  # regardless, and it is the only one of the two that knows about the platform
  # plugin, which is not a dependency of anything and is required all the same.
  #
  # The system libraries are dropped by a pattern with no backslash in it on
  # purpose. A backslash here has to survive CMake's string parsing before it
  # reaches the regex engine, and the version that did not silently matched
  # nothing, which shipped every DLL in System32 into the install tree.
  install(RUNTIME_DEPENDENCY_SET dv_client_runtime
    PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[Ss]ystem32.*"
    DIRECTORIES
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      "$<TARGET_FILE_DIR:Qt6::Core>"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

# Desktop integration, which is only a Linux idea: on Windows the installer
# writes the start menu entry and on macOS the bundle carries its own metadata.
# linuxdeploy reads both of these out of the installed tree to build the
# AppImage, so they are installed rather than passed to it on the command line.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES ${CMAKE_SOURCE_DIR}/assets/partyshare.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

  foreach(size 16 24 32 48 64 128 256 512)
    install(FILES ${CMAKE_SOURCE_DIR}/assets/partyshare-${size}.png
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/apps
      RENAME partyshare.png)
  endforeach()

  # The scalable icon is what a desktop at a fractional scale factor picks, and
  # leaving it out is how an icon ends up resampled and soft on a HiDPI screen.
  install(FILES ${CMAKE_SOURCE_DIR}/assets/partyshare.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
endif()
