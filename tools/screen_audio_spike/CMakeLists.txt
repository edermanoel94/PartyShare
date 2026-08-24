# Fase 0 do áudio da tela compartilhada. Ver docs/audio-da-tela-compartilhada.md.
#
# Não faz parte do produto: prova que AudioFrameProcessor é honrado por esta
# build do libwebrtc e que um frame estéreo devolvido por ele chega estéreo do
# outro lado. Enable com -DDV_ENABLE_SCREEN_AUDIO_SPIKE=ON.

# The client already found it when DV_BUILD_CLIENT_MEDIA is on, and asking a
# second time tries to declare the same imported target twice.
if(NOT TARGET libwebrtc::libwebrtc)
  find_package(libwebrtc REQUIRED)
endif()

# O modo mixer usa o misturador e o adaptador do produto, não uma cópia deles:
# provar a mistura com uma cópia provaria alguma coisa sobre a cópia. Os fontes
# entram aqui em vez de linkar dv::client_core porque essa biblioteca arrasta
# libdatachannel e o OpenSSL do vcpkg, que colide com o BoringSSL de dentro do
# webrtc.lib. É o mesmo motivo pelo qual dv_media_tests não linka nesta máquina.
add_executable(dv_screen_audio_spike
  main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../client/src/audio/screen_audio_mixer.cpp
)

if(WIN32)
  target_sources(dv_screen_audio_spike PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client/src/audio/loopback_capturer_windows.cpp
  )
  target_link_libraries(dv_screen_audio_spike PRIVATE ole32 mmdevapi)
else()
  target_sources(dv_screen_audio_spike PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client/src/audio/loopback_stub.cpp
  )
endif()

target_include_directories(dv_screen_audio_spike PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../client/src
)

# dv::shared for the logger only, and it pulls in nothing that clashes with
# libwebrtc: spdlog and fmt, no TLS of any kind.
target_link_libraries(dv_screen_audio_spike PRIVATE libwebrtc::libwebrtc dv::shared)

if(WIN32)
  # O spike chama WSAStartup por conta própria: InitializeSSL não abre o
  # Winsock, e sem ele o ICE não cria um socket sequer.
  target_link_libraries(dv_screen_audio_spike PRIVATE ws2_32)
endif()

set_target_properties(dv_screen_audio_spike PROPERTIES OUTPUT_NAME "screen-audio-spike")

# O conjunto estrito de avisos fica de fora pelo mesmo motivo do outro spike: os
# cabeçalhos do libwebrtc não passam nele, e este alvo é descartável.
dv_enable_sanitizers(dv_screen_audio_spike)
