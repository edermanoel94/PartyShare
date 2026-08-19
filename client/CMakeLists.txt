# The client is split in two on purpose.
#
# dv_client_core holds everything that is not the interface: signaling, media
# and, from M6, screen capture. It must never include a Qt header, which is the
# layering rule of section 15 of SPEC.md. Keeping it a library of its own is
# also what lets the tests drive it without starting a GUI.
#
# dv_client is the Qt executable on top of it.

add_library(dv_client_core STATIC
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
