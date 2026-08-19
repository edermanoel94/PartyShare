# SPEC.md

# Desktop Screen Sharing & Voice App

## 1. Objetivo

Construir um aplicativo desktop multiplataforma, desenvolvido em **C++**, capaz de realizar:

* Compartilhamento de tela em tempo real.
* Transmissão de áudio com boa qualidade.
* Comunicação em canais de voz.
* Suporte inicial para aproximadamente **5 participantes simultâneos**.
* Funcionamento em Windows, Linux e macOS.
* Geração de binários nativos para cada plataforma.

O projeto deve priorizar **baixa latência, estabilidade, baixo consumo de CPU/memória e qualidade de áudio/vídeo**.

---

## 2. Plataformas

O aplicativo deverá gerar binários para:

| Plataforma | Arquitetura | Status      |
| ---------- | ----------- | ----------- |
| Windows    | x64         | Obrigatório |
| Linux      | x64         | Obrigatório |
| macOS      | ARM64       | Obrigatório |
| macOS      | x64         | Desejável   |

O código deverá ser majoritariamente compartilhado entre as plataformas, evitando implementações específicas de sistema operacional sempre que possível.

---

## 3. Stack tecnológica

### Linguagem

* **C++20**
* Compilação com:

  * MSVC no Windows
  * GCC/Clang no Linux
  * Clang no macOS

### Build

Utilizar:

* CMake
* CMake Presets
* Ninja

Estrutura esperada:

```text
project/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
├── src/
├── include/
├── tests/
├── assets/
├── third_party/
└── docs/
```

---

## 4. Framework da interface

A interface gráfica deverá ser multiplataforma.

Preferência inicial:

**Qt 6**

Motivos:

* Suporte nativo a Windows, Linux e macOS.
* Boa integração com C++.
* Sistema de janelas, eventos e componentes de UI.
* Possibilidade de gerar aplicações nativas.
* Boa integração com threads e recursos multimídia.

A camada de UI deverá permanecer separada da camada responsável por captura, processamento e transmissão de mídia.

---

# 5. Funcionalidades principais

## 5.1 Sala / Canal

O aplicativo deverá permitir criar ou entrar em um canal.

Exemplo:

```text
Canal: sala-dev
ID: 8F42A1
```

Cada canal deverá permitir inicialmente:

* Até 5 participantes com áudio simultâneo.
* 1 participante compartilhando tela por vez.
* Entrada e saída dinâmica de participantes.

---

## 5.2 Compartilhamento de tela

O usuário deverá conseguir iniciar e parar o compartilhamento da tela.

Configuração inicial:

```text
Resolução: 1280x720
FPS: 30
```

Características:

* Captura da tela em tempo real.
* Seleção de monitor quando houver múltiplos monitores.
* Possibilidade futura de compartilhar apenas uma janela.
* Controle para iniciar/parar transmissão.
* Indicador visual de que o compartilhamento está ativo.

### Configuração inicial

```text
Resolution: 1280x720
FPS: 30
```

### Objetivos de qualidade

O sistema deverá priorizar:

1. Baixa latência.
2. Estabilidade do FPS.
3. Boa qualidade visual.
4. Uso eficiente de CPU.
5. Adaptação à largura de banda disponível.

---

# 6. Codec de vídeo

O sistema deverá utilizar inicialmente um codec moderno de vídeo.

Preferência:

**H.264**

Motivos:

* Excelente suporte multiplataforma.
* Hardware encoding disponível em muitas GPUs/CPUs.
* Boa relação qualidade/bitrate.
* Ecossistema maduro.

O sistema deverá ser arquitetado para permitir adicionar futuramente:

* VP9
* AV1

### Bitrate inicial

Configuração inicial sugerida:

```text
Resolution: 1280x720
FPS: 30
Bitrate: 1.5 Mbps ~ 3 Mbps
```

O bitrate deverá ser configurável e futuramente adaptativo.

---

# 7. Captura de tela

A captura deverá utilizar APIs nativas quando necessário.

### Windows

Preferência:

* Windows Graphics Capture

Fallback:

* Desktop Duplication API

### macOS

Utilizar:

* ScreenCaptureKit

### Linux

Suportar inicialmente:

* PipeWire
* X11

O suporte a Wayland deverá ser considerado desde o início da arquitetura.

---

# 8. Áudio

O aplicativo deverá permitir comunicação de voz em tempo real.

Requisitos:

* Microfone em tempo real.
* Reprodução dos participantes.
* Mute/unmute.
* Controle individual de volume.
* Seleção do dispositivo de entrada.
* Seleção do dispositivo de saída.
* Cancelamento de eco.
* Noise suppression.
* Automatic Gain Control, quando apropriado.

---

# 9. Codec de áudio

Utilizar:

**Opus**

Configuração inicial:

```text
Sample Rate: 48 kHz
Channels: Mono
Bitrate: 32 ~ 64 kbps
Frame Duration: 20 ms
```

O codec deverá suportar posteriormente áudio estéreo.

O Opus é preferível devido à baixa latência e boa qualidade em chamadas de voz.

---

# 10. Comunicação em tempo real

A arquitetura deverá separar:

```text
Application
    │
    ├── UI
    │
    ├── Screen Capture
    │
    ├── Audio Capture
    │
    ├── Video Encoder
    │
    ├── Audio Encoder
    │
    └── Network Layer
```

Para transporte de mídia em tempo real, avaliar:

**WebRTC**

O WebRTC deverá ser a principal alternativa para:

* Transporte de áudio.
* Transporte de vídeo.
* Controle de congestionamento.
* NAT traversal.
* Jitter buffering.
* RTP/RTCP.
* Criptografia da comunicação.

---

# 11. Arquitetura de rede

O sistema deverá possuir uma arquitetura cliente/servidor.

```text
                ┌──────────────┐
                │    Server    │
                │              │
                │ Signaling    │
                │ Media        │
                │ Room Manager │
                └──────┬───────┘
                       │
          ┌────────────┼────────────┐
          │            │            │
       Client A     Client B     Client C
```

O servidor deverá inicialmente ser responsável por:

* Autenticação.
* Criação de salas.
* Entrada/saída de usuários.
* Signaling.
* Gerenciamento dos participantes.
* Coordenação das conexões WebRTC.

---

# 12. Modelo de mídia

Para o MVP, utilizar arquitetura **SFU (Selective Forwarding Unit)**.

Exemplo:

```text
User A ───────┐
User B ───────┤
User C ───────┼──> SFU
User D ───────┤      │
User E ───────┘      │
                     │
             ┌───────┼───────┐
             │       │       │
             ▼       ▼       ▼
           User A  User B  User C
```

O SFU deverá encaminhar os streams sem realizar transcoding sempre que possível.

Isso reduz:

* Uso de CPU.
* Latência.
* Custo do servidor.

---

# 13. Signaling

O signaling será responsável por negociar as conexões.

Pode utilizar:

* WebSocket
* JSON

Exemplo:

```json
{
  "type": "join_room",
  "room_id": "8F42A1",
  "user_id": "user123"
}
```

Mensagens esperadas:

```text
join_room
leave_room
user_joined
user_left
offer
answer
ice_candidate
screen_share_started
screen_share_stopped
mute
unmute
```

---

# 14. Servidor

O servidor poderá inicialmente ser implementado em C++ para manter o ecossistema homogêneo.

Entretanto, a arquitetura deverá manter o protocolo independente da linguagem para permitir futuramente implementar o backend em outra linguagem.

Responsabilidades:

```text
Server
├── Authentication
├── Room Management
├── Signaling
├── SFU
├── User Management
└── Connection Management
```

---

# 15. Estrutura do projeto

```text
partyshare/
│
├── client/
│   ├── src/
│   │   ├── app/
│   │   ├── ui/
│   │   ├── audio/
│   │   ├── video/
│   │   ├── screen/
│   │   ├── network/
│   │   └── webrtc/
│   │
│   └── CMakeLists.txt
│
├── server/
│   ├── src/
│   │   ├── signaling/
│   │   ├── rooms/
│   │   ├── sfu/
│   │   └── network/
│   │
│   └── CMakeLists.txt
│
├── shared/
│   ├── protocol/
│   └── models/
│
├── tests/
│
├── cmake/
│
├── docs/
│
├── CMakeLists.txt
└── CMakePresets.json
```

---

# 16. Threads

O aplicativo não deverá realizar processamento pesado na thread principal da UI.

Sugestão:

```text
Main/UI Thread
      │
      ├── Audio Capture Thread
      │
      ├── Audio Processing Thread
      │
      ├── Screen Capture Thread
      │
      ├── Video Encoding Thread
      │
      ├── Network Thread
      │
      └── WebRTC Thread
```

A comunicação entre componentes deverá utilizar estruturas thread-safe.

Evitar:

* Locks excessivos.
* Operações bloqueantes.
* I/O na UI thread.
* Cópias desnecessárias de frames.

Preferir:

* Move semantics.
* RAII.
* Smart pointers.
* Lock-free queues quando justificável.
* Zero-copy quando possível.

---

# 17. Segurança

Toda comunicação de mídia deverá ser criptografada.

Utilizar as primitivas de segurança fornecidas pelo WebRTC sempre que possível.

O sistema deverá evitar:

* Comunicação de áudio/vídeo sem criptografia.
* Armazenamento desnecessário de streams.
* Credenciais em texto puro.
* Tokens persistidos sem proteção.

---

# 18. Identidade do usuário

Cada usuário deverá possuir:

```text
User
├── ID
├── Display Name
└── Avatar
```

Para o MVP, autenticação poderá ser simples.

Exemplo:

```text
Username
Password
```

Posteriormente poderão ser adicionados:

* OAuth.
* Google.
* GitHub.
* Login por e-mail.

---

# 19. Interface inicial

A aplicação deverá possuir pelo menos:

### Tela inicial

```text
┌─────────────────────────────────┐
│            PartyShare           │
│                                 │
│        [ Criar Sala ]           │
│                                 │
│        [ Entrar em Sala ]       │
│                                 │
└─────────────────────────────────┘
```

### Tela da sala

```text
┌────────────────────────────────────────────┐
│ Sala: 8F42A1                               │
├────────────────────────────────────────────┤
│                                            │
│              Screen Share                  │
│                                            │
│          ┌───────────────────┐             │
│          │                   │             │
│          │     1280x720      │             │
│          │                   │             │
│          └───────────────────┘             │
│                                            │
├────────────────────────────────────────────┤
│ Participants                               │
│                                            │
│ ● User 1     🔊                            │
│ ● User 2     🔊                            │
│ ● User 3     🔇                            │
│ ● User 4     🔊                            │
│ ● User 5     🔊                            │
│                                            │
├────────────────────────────────────────────┤
│ [🎤 Mute] [🖥 Share Screen] [🚪 Leave]    │
└────────────────────────────────────────────┘
```

---

# 20. MVP

A primeira versão deverá conter somente:

* [ ] Aplicação desktop C++.
* [ ] Windows.
* [ ] Linux.
* [ ] macOS.
* [ ] Criar sala.
* [ ] Entrar em sala.
* [ ] Até 5 usuários.
* [ ] Comunicação de voz.
* [ ] Mute/unmute.
* [ ] Screen sharing.
* [ ] 1280x720.
* [ ] 30 FPS.
* [ ] H.264.
* [ ] Opus.
* [ ] WebRTC.
* [ ] Signaling via WebSocket.
* [ ] SFU.
* [ ] Seleção de microfone.
* [ ] Seleção de dispositivo de saída.
* [ ] Seleção de monitor.
* [ ] Criptografia.
* [ ] Build automatizado para as três plataformas.

---

# 21. Fora do MVP

Não implementar inicialmente:

* Gravação de chamadas.
* Compartilhamento de arquivos.
* Chat.
* Streaming público.
* Mais de 5 participantes.
* Vídeo da webcam.
* Transmissão 4K.
* Background blur.
* Virtual camera.
* Virtual microphone.
* Noise suppression avançado baseado em IA.
* Mobile applications.

Esses recursos poderão ser adicionados posteriormente.

---

# 22. Performance

Objetivos iniciais:

| Métrica       |         Objetivo |
| ------------- | ---------------: |
| Screen Share  |         1280x720 |
| FPS           |               30 |
| Participantes |                5 |
| Audio         |           48 kHz |
| Audio codec   |             Opus |
| Video codec   |            H.264 |
| Latência      |   < 150 ms ideal |
| CPU           |    Baixo consumo |
| Memory        | < 500 MB cliente |
| Startup       |     < 3 segundos |

Os números deverão ser tratados como metas e validados através de benchmarks reais.

---

# 23. Observabilidade

O aplicativo deverá possuir logging estruturado.

Níveis:

```text
TRACE
DEBUG
INFO
WARN
ERROR
FATAL
```

Exemplo:

```text
[INFO] Connected to signaling server
[INFO] Joined room: 8F42A1
[INFO] Screen sharing started
[INFO] Video encoder: H264
[INFO] Resolution: 1280x720
[INFO] FPS: 30
```

Métricas importantes:

* FPS.
* Bitrate.
* Packet loss.
* RTT.
* Jitter.
* CPU usage.
* Memory usage.
* Audio latency.
* Video latency.
* Connection state.

---

# 24. Testes

Deverão existir:

### Unit tests

Testar:

* Protocolos.
* Room management.
* Serialização.
* Configurações.
* Estados da conexão.
* Gerenciamento de participantes.

### Integration tests

Testar:

* Signaling.
* WebRTC.
* Criação de salas.
* Entrada/saída de usuários.
* Audio pipeline.
* Video pipeline.

### Performance tests

Testar:

* 5 usuários.
* 720p/30 FPS.
* Diferentes condições de rede.
* Packet loss.
* High latency.
* CPU usage.
* Memory usage.

---

# 25. CI/CD

Utilizar GitHub Actions ou equivalente.

Pipeline:

```text
Push
 │
 ├── Format
 ├── Static Analysis
 ├── Build
 ├── Unit Tests
 ├── Integration Tests
 └── Package
```

Builds:

```text
Windows x64
Linux x64
macOS ARM64
macOS x64
```

Artefatos:

```text
Windows → .exe / installer
Linux   → AppImage
macOS   → .app / .dmg
```

---

# 26. Qualidade do código

O projeto deverá seguir:

* C++20.
* RAII.
* SOLID quando aplicável.
* Preferência por composição.
* Interfaces bem definidas.
* Separação entre UI e core.
* Tratamento explícito de erros.
* Smart pointers.
* `std::unique_ptr` por padrão.
* `std::shared_ptr` somente quando houver ownership compartilhado real.
* Evitar `new`/`delete` manual.
* Evitar estado global.
* Testes automatizados.

Ferramentas recomendadas:

```text
clang-format
clang-tidy
cppcheck
AddressSanitizer
UndefinedBehaviorSanitizer
```

---

# 27. Arquitetura de alto nível

```text
                    ┌─────────────────────┐
                    │      Qt 6 UI        │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │    Application      │
                    │       Core          │
                    └──────────┬──────────┘
                               │
        ┌──────────────────────┼──────────────────────┐
        │                      │                      │
        ▼                      ▼                      ▼
┌───────────────┐      ┌───────────────┐      ┌───────────────┐
│ Screen Capture│      │ Audio Capture │      │    Network    │
└───────┬───────┘      └───────┬───────┘      └───────┬───────┘
        │                      │                      │
        ▼                      ▼                      ▼
┌───────────────┐      ┌───────────────┐      ┌───────────────┐
│ Video Encoder │      │ Audio Encoder │      │    WebRTC     │
│     H.264     │      │     Opus      │      │               │
└───────┬───────┘      └───────┬───────┘      └───────┬───────┘
        │                      │                      │
        └──────────────────────┼──────────────────────┘
                               ▼
                         Internet / SFU
                               │
                  ┌────────────▼────────────┐
                  │     Signaling Server    │
                  │        + SFU            │
                  └─────────────────────────┘
```

---

# 28. Roadmap

## Fase 1 — Fundação

* [ ] CMake.
* [ ] Estrutura do projeto.
* [ ] Qt 6.
* [ ] Logging.
* [ ] Configuração.
* [ ] CI.
* [ ] Builds multiplataforma.

## Fase 2 — Áudio

* [ ] Captura de microfone.
* [ ] Reprodução.
* [ ] Opus.
* [ ] Mute.
* [ ] Seleção de dispositivos.
* [ ] WebRTC Audio Track.

## Fase 3 — Screen Share

* [ ] Captura Windows.
* [ ] Captura macOS.
* [ ] Captura Linux.
* [ ] Pipeline de frames.
* [ ] H.264.
* [ ] 720p.
* [ ] 30 FPS.
* [ ] WebRTC Video Track.

## Fase 4 — Networking

* [ ] Signaling server.
* [ ] WebSocket.
* [ ] ICE.
* [ ] STUN.
* [ ] TURN.
* [ ] SFU.
* [ ] Room management.

## Fase 5 — UI

* [ ] Login.
* [ ] Criar sala.
* [ ] Entrar em sala.
* [ ] Lista de participantes.
* [ ] Controles de áudio.
* [ ] Screen sharing.
* [ ] Status da conexão.

## Fase 6 — Hardening

* [ ] Testes.
* [ ] Benchmarks.
* [ ] Network simulation.
* [ ] Packet loss testing.
* [ ] Crash reporting.
* [ ] Security review.
* [ ] Performance optimization.

## Fase 7 — Release

* [ ] Windows installer.
* [ ] Linux AppImage.
* [ ] macOS .app.
* [ ] macOS .dmg.
* [ ] Code signing.
* [ ] Release automation.

---

# 29. Critérios de aceitação do MVP

O MVP será considerado funcional quando:

1. Um usuário conseguir criar uma sala.
2. Outros usuários conseguirem entrar utilizando um ID.
3. Até 5 usuários conseguirem permanecer conectados simultaneamente.
4. Todos conseguirem conversar utilizando áudio.
5. O áudio possuir baixa latência e qualidade adequada.
6. Um usuário conseguir iniciar o compartilhamento de tela.
7. Os demais participantes conseguirem visualizar a tela.
8. O compartilhamento funcionar em **1280x720 a 30 FPS**.
9. O sistema utilizar H.264 para vídeo.
10. O sistema utilizar Opus para áudio.
11. A comunicação utilizar WebRTC.
12. O sistema funcionar em Windows, Linux e macOS.
13. O projeto possuir builds automatizados para as plataformas suportadas.
14. O aplicativo permanecer responsivo durante captura, encoding e transmissão.
15. O sistema não depender de componentes específicos de uma única plataforma.

---

# 30. Princípio arquitetural

O projeto deverá ser construído pensando primeiro em **mídia em tempo real**, e não apenas como uma aplicação desktop tradicional.

A separação fundamental deverá ser:

```text
UI
 ↓
Application Core
 ↓
Media Pipeline
 ↓
WebRTC
 ↓
Network
 ↓
SFU
```

Dessa forma, a interface poderá evoluir independentemente do mecanismo de transmissão, permitindo posteriormente adicionar webcam, gravação, chat, compartilhamento de arquivos e aumentar o número de participantes sem precisar reescrever o núcleo do sistema.

