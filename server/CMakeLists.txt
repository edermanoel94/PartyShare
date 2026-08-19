find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# The protocol logic lives in a library so the tests can drive it directly,
# both with and without a real socket.
add_library(dv_server_core STATIC
  src/rooms/room_manager.cpp
  src/sfu/media_router.cpp
  src/sfu/video_feedback.cpp
  src/signaling/authenticator.cpp
  src/signaling/hub.cpp
  src/signaling/server.cpp
)
add_library(dv::server_core ALIAS dv_server_core)

target_include_directories(dv_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(dv_server_core
  PUBLIC dv::shared LibDataChannel::LibDataChannel
  PRIVATE OpenSSL::Crypto
)

dv_set_target_warnings(dv_server_core)
dv_enable_sanitizers(dv_server_core)

add_executable(dv_server src/main.cpp)

target_link_libraries(dv_server PRIVATE
  dv::server_core
  nlohmann_json::nlohmann_json
)

set_target_properties(dv_server PROPERTIES OUTPUT_NAME "partyshare-server")

dv_set_target_warnings(dv_server)
dv_enable_sanitizers(dv_server)

install(TARGETS dv_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
