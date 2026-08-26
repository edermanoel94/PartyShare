cmake_minimum_required(VERSION 3.25)

project(partyshare
  VERSION 0.1.28
  DESCRIPTION "Screen sharing and voice rooms for small groups"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(DV_BUILD_CLIENT "Build the desktop client" ON)
option(DV_BUILD_SERVER "Build the signaling and SFU server" ON)
option(DV_BUILD_TESTS "Build the test suite" ON)
option(DV_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DV_ENABLE_WEBRTC_SPIKE "Build the M3 libwebrtc toolchain spike" OFF)
option(DV_ENABLE_SCREEN_AUDIO_SPIKE "Build the shared screen audio spike" OFF)
# Off by default so that the whole suite builds and runs on a machine with no
# MongoDB and no mongo-cxx-driver. With it off the server keeps its accounts,
# rooms and audit log in memory, which is what it did before persistence
# existed. Configure with -DVCPKG_MANIFEST_FEATURES=mongo to get the driver.
option(DV_ENABLE_MONGO "Persist accounts, rooms and the audit log in MongoDB" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Every artefact the M9 builds is a different way of laying out the same
# installed tree, so the tree has to exist before any of them can be built.
include(GNUInstallDirs)

include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

add_subdirectory(shared)

if(DV_BUILD_CLIENT)
  add_subdirectory(client)
endif()

if(DV_BUILD_SERVER)
  add_subdirectory(server)
endif()

if(DV_ENABLE_WEBRTC_SPIKE)
  add_subdirectory(tools/webrtc_spike)
endif()

if(DV_ENABLE_SCREEN_AUDIO_SPIKE)
  add_subdirectory(tools/screen_audio_spike)
endif()

if(DV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(Packaging)

message(STATUS "")
message(STATUS "partyshare ${PROJECT_VERSION}")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  client          : ${DV_BUILD_CLIENT}")
message(STATUS "  server          : ${DV_BUILD_SERVER}")
message(STATUS "  tests           : ${DV_BUILD_TESTS}")
message(STATUS "  sanitizers      : ${DV_ENABLE_SANITIZERS}")
message(STATUS "  webrtc spike    : ${DV_ENABLE_WEBRTC_SPIKE}")
message(STATUS "  screen audio spike: ${DV_ENABLE_SCREEN_AUDIO_SPIKE}")
message(STATUS "  mongodb         : ${DV_ENABLE_MONGO}")
message(STATUS "")
