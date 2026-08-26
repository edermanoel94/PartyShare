find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# The protocol logic lives in a library so the tests can drive it directly,
# both with and without a real socket.
add_library(dv_server_core STATIC
  src/rooms/room_manager.cpp
  src/sfu/media_router.cpp
  src/sfu/video_feedback.cpp
  src/sfu/video_stitcher.cpp
  src/signaling/authenticator.cpp
  src/signaling/hub.cpp
  src/signaling/server.cpp
  src/store/memory_store.cpp
)
add_library(dv::server_core ALIAS dv_server_core)

target_include_directories(dv_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(dv_server_core
  PUBLIC dv::shared LibDataChannel::LibDataChannel
  PRIVATE OpenSSL::Crypto
)

if(DV_ENABLE_MONGO)
  find_package(mongocxx CONFIG REQUIRED)

  target_sources(dv_server_core PRIVATE src/store/mongo_store.cpp)
  target_compile_definitions(dv_server_core PUBLIC DV_HAS_MONGO=1)
  # PRIVATE: no public header of this library mentions the driver, which is
  # what store/mongo_store.hpp hiding its Impl behind a pointer is for. The
  # tests and the executable link the library without seeing bsoncxx at all.
  target_link_libraries(dv_server_core PRIVATE mongo::mongocxx_static)
endif()

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
