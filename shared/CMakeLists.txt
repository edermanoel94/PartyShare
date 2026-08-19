add_library(dv_shared STATIC
  src/logging/logger.cpp
  src/config/config.cpp
  src/models/room.cpp
  src/protocol/message.cpp
)
add_library(dv::shared ALIAS dv_shared)

target_include_directories(dv_shared PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# spdlog is public because dv/logging/logger.hpp exposes it through the macros.
# nlohmann_json stays private: no public header mentions it, which keeps the
# JSON library swappable.
target_link_libraries(dv_shared
  PUBLIC spdlog::spdlog
  PRIVATE nlohmann_json::nlohmann_json
)

# Compile out TRACE and DEBUG logging in optimized builds.
target_compile_definitions(dv_shared PUBLIC
  $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
  $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

dv_set_target_warnings(dv_shared)
dv_enable_sanitizers(dv_shared)
