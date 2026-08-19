# Dependency resolution.
#
# Every dependency is looked up with find_package first, so a toolchain that
# already provides it (vcpkg, a distro package, a system install) wins.
# FetchContent is only the fallback, which keeps a clean checkout buildable
# without requiring vcpkg to be installed.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

function(dv_log_dependency name origin)
  message(STATUS "Dependency ${name}: ${origin}")
endfunction()

# --- spdlog ------------------------------------------------------------------
find_package(spdlog 1.12 QUIET)
if(spdlog_FOUND)
  dv_log_dependency(spdlog "system")
else()
  dv_log_dependency(spdlog "FetchContent")
  set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.0
    GIT_SHALLOW TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(spdlog)
endif()

# --- nlohmann_json -----------------------------------------------------------
find_package(nlohmann_json 3.11 QUIET)
if(nlohmann_json_FOUND)
  dv_log_dependency(nlohmann_json "system")
else()
  dv_log_dependency(nlohmann_json "FetchContent")
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- GoogleTest --------------------------------------------------------------
if(DV_BUILD_TESTS)
  find_package(GTest 1.14 QUIET)
  if(GTest_FOUND)
    dv_log_dependency(GTest "system")
  else()
    dv_log_dependency(GTest "FetchContent")
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2
      GIT_SHALLOW TRUE
    SYSTEM
    )
    FetchContent_MakeAvailable(googletest)
  endif()
endif()

# --- libdatachannel ----------------------------------------------------------
# Carries the WebSocket on both ends of the signaling protocol, and the SFU in
# the server (M4). It is not header only and has no FetchContent fallback, so
# it has to come from vcpkg or from a system package.
if(DV_BUILD_CLIENT OR DV_BUILD_SERVER)
  find_package(LibDataChannel QUIET)
  if(LibDataChannel_FOUND)
    dv_log_dependency(LibDataChannel "system")
  else()
    message(FATAL_ERROR
      "libdatachannel was not found.\n"
      "Install it, or configure with the vcpkg toolchain:\n"
      "  cmake --preset <preset> "
      "-DCMAKE_TOOLCHAIN_FILE=$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake\n"
      "Or build only the shared library and its tests with "
      "-DDV_BUILD_CLIENT=OFF -DDV_BUILD_SERVER=OFF.")
  endif()
endif()
