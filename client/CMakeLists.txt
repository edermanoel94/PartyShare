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

# Capturing what an application is playing is a platform API and nothing else:
# it needs no libwebrtc, so it lives in the core rather than behind
# DV_BUILD_CLIENT_MEDIA. See docs/audio-da-tela-compartilhada.md.
if(WIN32)
  set(dv_loopback_sources
    src/audio/loopback_capturer_windows.cpp
    src/audio/audio_sources_windows.cpp
  )
else()
  set(dv_loopback_sources src/audio/loopback_stub.cpp)
endif()

# Playing a sound is a platform API, and the one Qt offers costs a module and
# its codec plugins for two chimes of a fifth of a second. See
# client/src/ui/sound.hpp.
#
# Notifying is not on this list any more: that goes through QSystemTrayIcon,
# which is portable and needs nothing here. docs/avisos-de-entrada-e-saida.md
# has what the native path did instead, and why it was dropped.
if(WIN32)
  set(dv_sound_sources src/ui/sound_windows.cpp)
else()
  set(dv_sound_sources src/ui/sound_stub.cpp)
endif()

add_library(dv_client_core STATIC
  ${dv_loopback_sources}
  src/audio/audio_sources.hpp
  src/audio/block_pacer.hpp
  src/audio/loopback_capturer.hpp
  src/audio/screen_audio_mixer.cpp
  src/audio/screen_audio_mixer.hpp
  src/app/call_session.cpp
  src/app/call_session.hpp
  src/app/chat_links.cpp
  src/app/chat_links.hpp
  src/app/metrics_history.cpp
  src/app/metrics_history.hpp
  src/app/network_quality.hpp
  src/app/smoothing.hpp
  src/media/media_session.cpp
  src/media/media_session.hpp
  src/media/network_impairment.cpp
  src/media/network_impairment.hpp
  src/network/signaling_client.cpp
  src/network/signaling_client.hpp
  src/network/transport_globals.cpp
  src/network/transport_globals.hpp
  src/video/frame_queue.hpp
  src/video/frame_size.hpp
  src/video/screen_quality.hpp
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

if(WIN32)
  # ole32 for COM, mmdevapi for ActivateAudioInterfaceAsync and the device
  # enumerator. Both are part of Windows; nothing is downloaded for this.
  target_link_libraries(dv_client_core PRIVATE ole32 mmdevapi)
endif()

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
    src/webrtc/dynamic_library.cpp
    src/webrtc/dynamic_library.hpp
    src/webrtc/hardware_encoder.cpp
    src/webrtc/hardware_encoder.hpp
    src/webrtc/hardware_encoder_backend.hpp
    src/webrtc/hardware_encoder_mediafoundation.cpp
    src/webrtc/hardware_encoder_nvenc.cpp
    src/webrtc/impaired_socket_factory.cpp
    src/webrtc/impaired_socket_factory.hpp
    src/webrtc/video_encoder_factory.cpp
    src/webrtc/video_encoder_factory.hpp
    src/webrtc/libwebrtc_media_session.cpp
    src/webrtc/libwebrtc_screen_capturer.cpp
    src/webrtc/screen_audio_frame_processor.hpp
  )

  target_include_directories(dv_client_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  # Only for dlopen, which webrtc/dynamic_library.cpp needs whether or not a
  # backend is built on top of it. Empty on the platforms that keep it in libc,
  # and on Windows, where the loader is LoadLibrary and lives in kernel32.
  target_link_libraries(dv_client_media PRIVATE ${CMAKE_DL_LIBS})

  # Not on Apple: NVENC needs an NVIDIA driver, and macOS has had no NVIDIA
  # driver since Mojave. VideoToolbox is the answer there and is not written
  # yet.
  #
  # Windows is in, since the port of the loader. The API above it is the same
  # on both systems; what differed was dlopen against LoadLibrary and the file
  # names of the two driver libraries.
  if(DV_HARDWARE_ENCODER_NVENC AND NOT APPLE)
    target_compile_definitions(dv_client_media PRIVATE DV_WITH_NVENC)
    target_include_directories(dv_client_media PRIVATE
      ${CMAKE_SOURCE_DIR}/third_party/nvcodec)
    list(APPEND dv_hardware_encoders NVENC)
  endif()

  # Windows only, and the reason it exists next to NVENC rather than instead of
  # it: NVENC reaches NVIDIA cards and nothing else, and Media Foundation is
  # how Intel QuickSync and AMD VCN are reached. A build carries both and the
  # machine that runs it decides, because a compile-time choice cannot know
  # which card is in the machine the binary ends up on.
  option(DV_HARDWARE_ENCODER_MEDIA_FOUNDATION
         "Build the Media Foundation hardware encoder backend" ON)

  if(DV_HARDWARE_ENCODER_MEDIA_FOUNDATION AND WIN32)
    target_compile_definitions(dv_client_media PRIVATE DV_WITH_MEDIA_FOUNDATION)
    # GUID constants only. These are static libraries of bytes, not import
    # libraries, so linking them adds no DLL the executable needs at startup -
    # which matters, because mfplat.dll itself is deliberately not linked.
    target_link_libraries(dv_client_media PRIVATE mfuuid strmiids)
    list(APPEND dv_hardware_encoders "Media Foundation")
  endif()

  if(dv_hardware_encoders)
    list(JOIN dv_hardware_encoders ", " dv_hardware_encoder_list)
    message(STATUS "Hardware encoders: ${dv_hardware_encoder_list}")
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
  ${dv_sound_sources}
  src/main.cpp
  src/ui/admin_panel.cpp
  src/ui/admin_panel.hpp
  src/ui/chat_view.cpp
  src/ui/chat_view.hpp
  src/ui/chimes.cpp
  src/ui/chimes.hpp
  src/ui/main_window.cpp
  src/ui/main_window.hpp
  src/ui/metrics_chart.cpp
  src/ui/metrics_chart.hpp
  src/ui/metrics_dialog.cpp
  src/ui/metrics_dialog.hpp
  src/ui/notifier.cpp
  src/ui/notifier.hpp
  src/ui/sound.hpp
  src/ui/screen_view.cpp
  src/ui/screen_view.hpp
  src/ui/settings_dialog.cpp
  src/ui/settings_dialog.hpp
  src/ui/table.cpp
  src/ui/table.hpp
  src/ui/theme.cpp
  src/ui/theme.hpp
)

# The chevrons the stylesheet draws into combo boxes and spin boxes, at the
# three densities Qt looks for. See assets/ui/make_arrows.py.
set(dv_client_ui_images)
foreach(dv_chevron chevron-down chevron-up)
  foreach(dv_density "" "@2x" "@3x")
    list(APPEND dv_client_ui_images
      ${CMAKE_SOURCE_DIR}/assets/ui/${dv_chevron}${dv_density}.png)
  endforeach()
endforeach()

# The same commented file that gets installed, compiled into the executable.
#
# It is what the client writes to the user's configuration directory the first
# time it runs, and that has to work on a machine where the installed copy is
# somewhere this process cannot find or cannot read - inside a signed .app, or
# under a prefix the packager moved. Compiled in, there is one copy of the text
# and it is always there.
qt_add_resources(dv_client "dv_client_assets"
  PREFIX "/"
  BASE ${CMAKE_SOURCE_DIR}/assets
  FILES
    ${CMAKE_SOURCE_DIR}/assets/config.ini
    ${CMAKE_SOURCE_DIR}/assets/sounds/joined.wav
    ${CMAKE_SOURCE_DIR}/assets/sounds/left.wav
    ${dv_client_ui_images}
)

target_link_libraries(dv_client PRIVATE
  dv::client_core
  Qt6::Widgets
)

if(WIN32)
  # winmm is PlaySound, which is the whole of the arrival and departure chime.
  # It ships with Windows; nothing is downloaded for this. See
  # client/src/ui/sound.hpp for why it is not QSoundEffect.
  target_link_libraries(dv_client PRIVATE winmm)
endif()

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

# The bundle carries its own Info.plist rather than CMake's, for one key that
# the default template has no place for: NSMicrophoneUsageDescription. macOS
# refuses the microphone outright to a bundle that does not carry it, without
# asking anybody, and a voice room then runs on silence with nothing in any log
# to say why. Configured here so the identity keys cannot drift from the
# properties above. See assets/Info.plist.in.
if(APPLE)
  set(MACOSX_BUNDLE_BUNDLE_NAME "PartyShare")
  set(MACOSX_BUNDLE_GUI_IDENTIFIER "dev.partyshare.client")
  set(MACOSX_BUNDLE_EXECUTABLE_NAME "partyshare")
  set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_ICON_FILE "partyshare.icns")
  configure_file(${CMAKE_SOURCE_DIR}/assets/Info.plist.in
    ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
  set_target_properties(dv_client PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
endif()

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

# The file the client looks for on its own, installed under the name it looks
# for rather than as an example to be renamed.
#
# It used to be config.example.ini, on the grounds that the installer replaces
# what it installs and would take somebody's edited config.ini with it. That
# reasoning was sound and the conclusion was not: the answer is not to ship a
# file nobody can use without a rename, it is to keep what a person changes out
# of the installer's reach. Which is now the case - every setting the interface
# writes goes to the config.ini under the user's own directory, which the client
# creates on first run and no installer touches.
#
# Every line in the shipped file is commented out, so installing it changes no
# behaviour. It is there to be read and uncommented.
if(WIN32)
  # Beside the executable, which is the directory the client actually reads.
  install(FILES ${CMAKE_SOURCE_DIR}/assets/config.ini
    DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
else()
  # Not bindir here: that directory is for executables on a Unix system, and
  # the two people who would look for a configuration file there are
  # outnumbered by the packaging tools that would complain about it.
  install(FILES ${CMAKE_SOURCE_DIR}/assets/config.ini
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
  )
endif()

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
