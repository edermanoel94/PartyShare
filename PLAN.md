# PLAN.md

Plano de implementação derivado de [SPEC.md](SPEC.md).

O plano é organizado em marcos (M0 a M9).
Cada marco tem entregáveis, tarefas e critérios de aceitação verificáveis.
A ordem não segue exatamente as fases da SPEC: os riscos técnicos maiores (toolchain de WebRTC e a primeira fatia vertical de mídia) foram puxados para o início, porque são eles que determinam se o resto do plano é viável.

---

## 1. Decisões de stack

Estas decisões precisam ser fechadas antes do M3, porque tudo depois depende delas.

| Área | Decisão | Motivo |
| --- | --- | --- |
| Linguagem | C++20 | Definido na SPEC. |
| Build | CMake + CMakePresets + Ninja | Definido na SPEC. |
| UI | Qt 6 (Widgets) | Definido na SPEC. Widgets em vez de QML porque a UI é densa em controles e não precisa de animação pesada. |
| WebRTC no cliente | libwebrtc (binários pré-compilados) | Entrega AEC3, noise suppression, AGC, jitter buffer, congestion control, SRTP, ICE e `modules/desktop_capture` prontos. Reimplementar qualquer um desses com qualidade equivalente não é realista. |
| Captura de tela | `webrtc::DesktopCapturer` | Já cobre Windows Graphics Capture, DXGI Desktop Duplication, ScreenCaptureKit, X11 e PipeWire, exatamente o que a seção 7 da SPEC pede. |
| Codec de vídeo | H.264 via OpenH264 (software) no MVP | Disponível dentro da libwebrtc, multiplataforma, sem dependência de GPU. Aceleração por hardware entra no M8 atrás de uma interface de encoder. |
| Codec de áudio | Opus via libwebrtc | Definido na SPEC. |
| SFU no servidor | libdatachannel | Biblioteca C++ pequena, compilável em qualquer plataforma, com DTLS-SRTP, ICE e encaminhamento de RTP no nível de pacote. A libwebrtc não é feita para encaminhar mídia de N para N. |
| Signaling | WebSocket + JSON | Definido na SPEC. O WebSocket vem do libdatachannel, não do Boost.Beast: é a mesma biblioteca que o SFU vai usar no M4, então o servidor tem uma pilha de rede só em vez de duas. nlohmann::json para as mensagens. |
| Logging | spdlog | Logging estruturado com os níveis da seção 23. |
| Testes | GoogleTest | Unitários e de integração. |
| Dependências | vcpkg em modo manifest | Reproduz o mesmo conjunto de versões nas três plataformas e no CI. libwebrtc entra como pacote binário externo, fora do vcpkg. |

### Risco principal, e o que o M3 já apurou

A libwebrtc não tem release oficial em forma de biblioteca.
O plano usa builds pré-compilados públicos, com versão fixada e verificada por checksum, mais um `Findlibwebrtc.cmake` próprio.

O M3 já foi executado no Linux e o resultado está em [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).
O spike compila, linka e roda: cria `PeerConnectionFactory`, gera SDP offer com Opus e H.264, e enumera dispositivos de áudio.

O spike também revelou um conflito que muda o plano.
No Linux os binários publicados são compilados contra a libc++ do Chromium, com namespace de ABI `std::__Cr`, enquanto o Qt 6 das distribuições usa libstdc++.
Como a API pública da libwebrtc troca `std::string` e `std::vector` o tempo todo, um único binário não pode usar as duas.

**Decisão tomada: compilar a libwebrtc a partir do fonte com `use_custom_libcxx=false`.**
`scripts/build_webrtc.sh` automatiza isso e empacota o resultado no layout que o `Findlibwebrtc.cmake` consome.
O build foi concluído no Linux e o spike passa sobre ele, então essa saída deixou de ser hipótese.
O projeto passa a ser responsável por manter e distribuir esse binário nas três plataformas, o que é o custo aceito em troca de não ter uma camada de tradução em C no meio de todo o pipeline de mídia.

Alternativa caso a libwebrtc se mostre inviável: usar libdatachannel também no cliente, somando libopus, `webrtc-audio-processing` standalone, OpenH264 e implementações próprias de captura de tela e de estimativa de banda.
Isso é significativamente mais trabalho e entrega menos qualidade, então só deve ser adotado como último recurso.

---

## 2. Estrutura do repositório

Segue a seção 15 da SPEC, com pequenos acréscimos.

```text
tudo-puta/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── .clang-format
├── .clang-tidy
├── cmake/
│   ├── Findlibwebrtc.cmake
│   ├── CompilerWarnings.cmake
│   └── Sanitizers.cmake
├── shared/
│   ├── protocol/          # mensagens de signaling e serialização
│   └── models/            # User, Room, Participant
├── client/
│   └── src/
│       ├── app/           # Application Core, estado da sessão
│       ├── ui/            # Qt 6, sem lógica de mídia
│       ├── audio/         # captura, playback, dispositivos
│       ├── video/         # encoder, decoder, render
│       ├── screen/        # DesktopCapturer e seleção de monitor
│       ├── network/       # cliente WebSocket de signaling
│       └── webrtc/        # PeerConnection, tracks, adaptadores
├── server/
│   └── src/
│       ├── signaling/     # WebSocket, roteamento de mensagens
│       ├── rooms/         # ciclo de vida de salas e participantes
│       ├── sfu/           # encaminhamento de RTP
│       └── network/       # ICE, DTLS, sockets
├── tests/
│   ├── unit/
│   ├── integration/
│   └── perf/
├── assets/
├── docs/
└── .github/workflows/
```

Regra de dependência, verificada em revisão de código: `ui` depende de `app`, e `app` depende dos módulos de mídia e rede.
Nenhum módulo de mídia ou rede pode incluir um header de Qt.

---

## 3. Marcos

### M0 — Fundação do build

Objetivo: um repositório que compila, roda testes e faz lint nas três plataformas, ainda sem nenhuma feature.

Tarefas:

1. `CMakeLists.txt` raiz com C++20, alvos `client`, `server`, `shared` e `tests`.
2. `CMakePresets.json` com presets `linux-debug`, `linux-release`, `windows-debug`, `windows-release`, `macos-arm64-debug`, `macos-arm64-release`, além de um preset com AddressSanitizer e UndefinedBehaviorSanitizer.
3. `vcpkg.json` com spdlog, nlohmann-json, gtest, boost-beast, libdatachannel.
4. `.clang-format` e `.clang-tidy`, e um alvo `format-check`.
5. Módulo de logging em `shared/`, com os níveis TRACE a FATAL da seção 23.
6. Carregamento de configuração (arquivo mais variáveis de ambiente mais argumentos de linha de comando), com valores padrão de resolução, FPS, bitrate e endereço do servidor.
7. Janela Qt 6 mínima, apenas para provar que o link com Qt funciona em todas as plataformas.
8. GitHub Actions com a matriz Windows x64, Linux x64 e macOS ARM64, executando format, build e testes unitários.

Critérios de aceitação:

- `cmake --preset <plataforma>-release && cmake --build --preset <plataforma>-release` funciona nas três plataformas.
- O CI está verde nos três runners.
- A janela Qt abre e fecha em todas elas.

Nota sobre o ambiente local: o Ninja não está instalado nesta máquina, o Qt 6 e o CMake 4.4 estão.
Instalar o Ninja é pré-requisito do M0.

---

### M1 — Protocolo compartilhado

Objetivo: o contrato de signaling existe, é testado e é independente de linguagem.

Tarefas:

1. Definir os tipos de mensagem da seção 13: `join_room`, `leave_room`, `user_joined`, `user_left`, `offer`, `answer`, `ice_candidate`, `screen_share_started`, `screen_share_stopped`, `mute`, `unmute`.
2. Adicionar as mensagens que faltam na SPEC mas que o fluxo exige: `create_room`, `room_created`, `error`, `ping` e `pong`.
3. Structs C++ com serialização e desserialização para JSON, retornando erro explícito em vez de exceção.
4. Modelos em `shared/models`: `User` (id, display name, avatar), `Room`, `Participant`.
5. Documentar o protocolo em `docs/protocol.md`, incluindo a máquina de estados da sessão.
6. Testes unitários de ida e volta, de campos ausentes, de tipos errados e de JSON malformado.

Critérios de aceitação:

- Cobertura de teste em todas as mensagens, incluindo os casos de entrada inválida.
- `docs/protocol.md` descreve o protocolo sem depender de nenhum detalhe de C++.

---

### M2 — Servidor de signaling (concluído)

Objetivo: salas funcionando de ponta a ponta, ainda sem mídia.

Tarefas:

1. [x] Servidor WebSocket com `rtc::WebSocketServer` do libdatachannel, em vez de Boost.Beast.
   A troca elimina uma pilha de rede inteira: a mesma biblioteca já é a escolhida para o SFU do M4.
2. [x] Autenticação de MVP: contas em memória com salt e SHA-256, token de sessão com expiração.
   As mensagens `authenticate` e `authenticated` foram acrescentadas ao protocolo.
3. [x] `RoomManager`: criar sala, ID hexadecimal de 6 caracteres, entrar, sair, limite de participantes, remoção da sala vazia.
4. [x] Roteamento das mensagens entre participantes, com o servidor encaminhando SDP e ICE sem interpretá-los.
5. [x] Heartbeat com `ping` e `pong`, e remoção do participante por timeout.
6. [x] Regra de um compartilhamento de tela por vez, aplicada no servidor.
7. [x] Testes de integração com clientes WebSocket reais contra o servidor em porta efêmera.

Critérios de aceitação, todos verificados:

- [x] Cinco clientes entram na mesma sala e todos recebem os `user_joined` corretos, e o sexto recebe `room_full`.
- [x] Derrubar um cliente sem handshake gera `user_left` para os demais bem dentro dos 5 segundos.
- [x] O servidor sobrevive a JSON malformado, tipo desconhecido e campo obrigatório ausente, e continua servindo.

Além do previsto, o servidor valida que `user_id` e `from_user_id` correspondem à identidade autenticada da conexão.
Sem isso, qualquer participante poderia mutar outro ou enviar uma offer em nome alheio.

---

### M3 — Spike de toolchain WebRTC (parcialmente concluído)

Objetivo: eliminar o maior risco do projeto antes de construir em cima dele.
Este marco é curto e descartável se falhar.

Tarefas:

1. [x] Escolher a distribuição de binários da libwebrtc e fixar a versão (`m152.7977.0.0`).
2. [x] Escrever `cmake/Findlibwebrtc.cmake`, com download, verificação de checksum e exposição de um alvo importado.
3. [x] Programa mínimo que cria uma `PeerConnection`, gera uma offer e imprime o SDP.
4. [x] Verificar no mesmo binário que `DesktopCapturer` e `AudioDeviceModule` enumeram monitores e dispositivos.
5. [x] Documentar em `docs/webrtc-toolchain.md` como reproduzir o build do zero.
6. [x] Decidir o conflito de biblioteca padrão, e automatizar a solução em `scripts/build_webrtc.sh`.
7. [x] Fazer o spike detectar o conflito sozinho, linkando `dv::shared` e passando uma `std::string` pela fronteira.
8. [x] Concluir o build do fonte e revalidar o spike sobre ele.
9. [x] Validar a captura de tela com servidor gráfico anexado, em X11.
10. [ ] Repetir a validação da captura em uma sessão Wayland.
11. [ ] Rodar o spike no Windows x64 e no macOS ARM64, seguindo `docs/webrtc-validation.md`.

Critérios de aceitação:

- O binário mínimo roda em Windows, Linux e macOS ARM64.
  Linux confirmado sobre o build do fonte, os outros dois pendentes.
- O CI baixa e faz cache da libwebrtc sem intervenção manual.
  O job existe em `.github/workflows/ci.yml` sob `workflow_dispatch`, ainda não executado.
- Sem esses dois itens, o plano volta para a alternativa descrita na seção 1.

O que o build do fonte apurou está em [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).
O conflito de biblioteca padrão acabou: a biblioteca resultante só tem símbolos em `std::__cxx11`, a mesma ABI do Qt, e o spike passa com o `dv::shared` linkado.
Ele também captura um frame de 1920x1080 de verdade em X11, o que é a garantia que o M6 precisa antes de existir.

---

### M4 — Fatia vertical: áudio entre dois clientes

Objetivo: a menor coisa que exercita UI, core, mídia, signaling e SFU juntos.
É aqui que a arquitetura é validada de verdade.

Tarefas:

1. [x] Cliente de signaling em `client/src/network`, rodando fora da thread de UI.
2. [x] Camada `client/src/webrtc`: criação de `PeerConnection`, negociação, coleta de candidatos ICE, track de áudio Opus a 48 kHz mono, 20 ms.
3. [x] SFU mínimo em `server/src/sfu`: uma sessão libdatachannel por participante, encaminhamento de RTP de áudio para todos os outros, reescrita de SSRC, encaminhamento de relatórios RTCP.
4. [x] Servidor STUN configurável, com TURN previsto na configuração mas ainda não obrigatório.
5. [x] UI provisória: campo de ID de sala, botão de entrar, botão de mute.
6. [x] Métricas básicas no log: RTT, jitter, packet loss e bitrate, a cada 5 segundos.

Decisão de topologia, tomada aqui e registrada na seção 4.3 de [docs/protocol.md](docs/protocol.md): **quem oferece é sempre o servidor**.
O participante só responde.
Isso deixa mids, SSRCs e payload types sob controle do SFU, e reduz o encaminhamento a reescrever um cabeçalho em vez de traduzir entre duas negociações independentes.
O identificador reservado `sfu` é o endereço desse ponto de mídia, e uma mensagem endereçada a ele é consumida pelo servidor em vez de retransmitida.

A camada de mídia do cliente fica atrás da interface `audio::AudioSession`, e a implementação sobre a libwebrtc é uma biblioteca à parte, ligada por `-DDV_BUILD_CLIENT_MEDIA=ON`.
Sem ela o cliente ainda compila e roda, e é isso que mantém o servidor, os testes e o CI livres de uma biblioteca de 66 MB que precisa ser construída do fonte.
Também é o que permite testar toda a ordem de operações de uma chamada com uma mídia de mentira, sem placa de som.

O que já está verificado por teste de integração, com ICE, DTLS e RTP de verdade sobre a loopback:

- O servidor oferece assim que o participante entra na sala.
- Cada participante recebe uma track por outro participante, com `a=msid` identificando de quem é a voz.
- O áudio de um participante chega ao outro, e não volta para quem enviou.
- Sair da sala remove a sessão e a track correspondente nos demais.
- **Um cliente libwebrtc negocia com o SFU libdatachannel, conecta e entrega áudio.**
  Esse era o risco alto listado na seção 5, e está retirado.

### Bug de captura do PulseAudio, encontrado aqui e corrigido

A primeira chamada de um processo funcionava e toda chamada seguinte demorava exatos dez segundos para negociar, sem microfone.

Causa: `AudioDeviceLinuxPulse::Terminate()` marca `quit_ = true` e o `Init()` nunca limpa essa flag, então a thread de captura da segunda sessão morre na primeira passagem e o `StartRecording()` espera dez segundos por um evento que ninguém vai sinalizar.
É um bug da libwebrtc, não do projeto.

Corrigido por `patches/webrtc/src/0002-pulse-adm-reset-quit-on-init.patch`, detalhado na seção 5.2 de [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).
Sessões sucessivas no mesmo processo passaram de 10,2 s para 1,2 s.

Critérios de aceitação:

- Dois clientes em máquinas diferentes se ouvem.
- O mute funciona nos dois sentidos.
- A latência de áudio boca a ouvido fica abaixo de 150 ms em rede local.
- A UI não trava durante a chamada.

O caminho completo foi exercitado na aplicação real, com dois clientes na mesma máquina e servidor local:

```text
ana:    em chamada    rtt 1 ms · jitter 2.0 ms · perdidos 0 · 97 kbps ↑ · 95 kbps ↓
bruno:  em chamada    rtt 1 ms · jitter 2.0 ms · perdidos 0 · 81 kbps ↑ · 82 kbps ↓
```

Cada um vê o outro na lista de participantes marcado como falando, e mutar um deles derruba o próprio envio para 1 kbps e a recepção do outro junto, com a lista dos dois mostrando o estado.

Falta o que exige duas máquinas e instrumentação: medir a latência boca a ouvido em rede local, que é o terceiro critério.

---

### M5 — Áudio completo

Objetivo: atender integralmente as seções 8 e 9 da SPEC.

Tarefas:

1. [x] Escalar para 5 participantes, com uma track de recepção por participante remoto.
2. [x] Enumeração e troca de dispositivos de entrada e de saída em tempo de execução, sem derrubar a chamada.
3. [x] Controle de volume individual por participante.
4. [x] Habilitar e ajustar AEC3, noise suppression e AGC.
5. [x] Indicador de nível de áudio e detecção de quem está falando.
6. [x] Propagação do estado de mute pelo signaling, para que a UI de todos fique consistente.
7. [x] Testes de integração do pipeline de áudio com um dispositivo virtual, para rodar no CI sem hardware.

Critérios de aceitação:

- Cinco participantes conversam simultaneamente sem eco perceptível.
  Cinco sessões e vinte tracks estão verificadas por teste; a ausência de eco depende do AEC3, que está ligado e é verificado abaixo, mas julgar "perceptível" pede cinco pessoas em cinco máquinas.
- [x] Trocar de microfone durante a chamada não causa corte maior que 500 ms.
  Medido pelo que o servidor recebe, não pelo que o cliente acha que fez.
- [x] O uso de CPU do cliente fica em um dígito percentual em uma máquina de referência.
  Medido em 9,2% de um núcleo em chamada, com AEC3, noise suppression e AGC ativos, em um Ryzen de 16 threads.

### Processamento de áudio, e como saber que está ligado

Pedir AEC3, noise suppression e AGC é uma linha de configuração.
Saber que eles estão rodando é outra coisa, e um teste que só lê a configuração de volta não prova nada.

A libwebrtc só publica `echo_return_loss` enquanto o controlador de eco está de fato processando captura.
É essa a métrica que `AudioStats::echo_cancellation_active` carrega, e são dois testes que a usam: um exige que o cancelador esteja rodando, o outro desliga a opção e exige que ele pare.
Sem o segundo, o primeiro passaria mesmo que a métrica não tivesse relação com o que o projeto pediu.

O módulo de processamento pertence à fábrica, não à conexão, então as sessões de um mesmo processo o compartilham e valem as últimas opções aplicadas.
Um processo de cliente tem um usuário local, então isso não muda nada no produto, mas muda os testes: o caso que desliga o cancelador roda sozinho.

### Bug do indicador de nível, encontrado aqui e corrigido

A barra de nível do microfone nunca saiu de zero, e o teste com dispositivo virtual foi o que mostrou isso.

Causa: `AudioTrackInterface::GetSignalLevel` é o caminho óbvio para perguntar, mas a fonte de uma track local nunca o implementa, então ele responde `false` para toda chamada e quem constrói um indicador em cima recebe silêncio para sempre.
O nível local agora vem de `RTCAudioSourceStats::audio_level`, que é onde a libwebrtc realmente o publica.

A barra também passou a ser lida em decibéis.
A fala normal fica perto de um vigésimo da escala cheia, e em escala linear mal levantaria a barra do chão.

### Travamento visto uma vez, não reproduzido

Em uma execução da suíte de mídia o caso `TheEchoCancellerRunsOnTheCapturedAudio` travou até o limite de 180 segundos do ctest, em vez de falhar nos 20 segundos que o próprio teste espera.
Travar além do tempo do teste só é possível dentro de uma chamada bloqueante da libwebrtc, e o suspeito é o dispositivo PulseAudio: a execução veio logo depois de dois clientes gráficos que capturavam áudio terem sido mortos.

Não reproduziu.
Cinco execuções completas da suíte em seguida, mais o caso isolado, mais três tentativas matando um cliente durante a captura e rodando o teste na sequência, todas passaram.
Fica registrado aqui em vez de esquecido: se voltar, o lugar de olhar é a abertura do dispositivo de captura, não o teste.

### Dispositivo de áudio virtual

`scripts/virtual_audio.sh` monta uma placa de som virtual sobre o PulseAudio, com um tom tocando no microfone, e é isso que permite rodar o pipeline inteiro em um runner sem hardware.
O job `media` do CI usa exatamente esse script.

Um dispositivo nulo não serviria: ele deixa a negociação passar, mas captura silêncio, e é justamente o áudio capturado que o M5 precisa verificar.
O microfone virtual é um `module-remap-source` sobre o monitor de um sink nulo, porque o backend PulseAudio da libwebrtc ignora toda fonte que monitora um sink ao enumerar dispositivos de captura.

---

### M6 — Compartilhamento de tela

Objetivo: atender as seções 5.2, 6 e 7 da SPEC.

Tarefas:

1. Interface `ScreenCapturer` própria, envolvendo o `DesktopCapturer` e isolando o resto do código do detalhe de plataforma.
2. Enumeração de monitores e seleção pela UI.
3. Pipeline de frames com fila limitada e descarte do frame mais antigo sob pressão, usando move semantics e evitando cópias.
4. Escala para 1280x720 e limitação a 30 FPS.
5. Encoder H.264 atrás de uma interface `VideoEncoder`, para que VP9 e AV1 possam ser acrescentados depois sem tocar no pipeline.
6. Bitrate configurável entre 1.5 e 3 Mbps, com o gancho de adaptação já presente mas desligado.
7. Encaminhamento de vídeo no SFU, incluindo tratamento de PLI e de keyframes para quem entra no meio da transmissão.
8. Renderização do vídeo recebido em um widget Qt, fora da thread de UI para decodificação.
9. Indicador visual de compartilhamento ativo e controle de iniciar e parar.

Critérios de aceitação:

- Um participante compartilha e os outros quatro veem, em 1280x720 a 30 FPS.
- Um participante que entra depois recebe um keyframe em menos de 2 segundos.
- Parar e reiniciar o compartilhamento funciona sem reiniciar a chamada.
- O FPS permanece estável com desvio pequeno durante 10 minutos.

---

### M7 — Interface final

Objetivo: a UI da seção 19, agora sobre um core já funcional.

Tarefas:

1. Tela de login.
2. Tela inicial com criar sala e entrar em sala.
3. Tela da sala: área de compartilhamento, lista de participantes com estado de áudio, barra de controles com mute, compartilhar tela e sair.
4. Diálogo de configurações: dispositivo de entrada, dispositivo de saída, monitor, bitrate.
5. Indicador de estado da conexão e de qualidade da rede.
6. Tratamento visual de erros: servidor fora do ar, sala cheia, sala inexistente, permissão de captura negada.
7. Reconexão automática ao signaling com backoff exponencial.

Critérios de aceitação:

- Todos os fluxos da seção 19 são navegáveis sem passar pelo terminal.
- Nenhuma operação bloqueante roda na thread de UI, verificado com profiler.
- O startup fica abaixo de 3 segundos.

---

### M8 — Hardening

Objetivo: transformar as metas da seção 22 em números medidos.

Tarefas:

1. Testes de performance com 5 participantes em 720p a 30 FPS, medindo CPU, memória e latência.
2. Simulação de rede com perda de pacotes, latência alta e jitter, usando `tc netem` no Linux.
3. Ativar a adaptação de bitrate com base no feedback de congestion control.
4. Encoders por hardware: Media Foundation ou NVENC no Windows, VideoToolbox no macOS, VAAPI no Linux, todos atrás da interface `VideoEncoder`, com fallback automático para software.
5. Rodar a suíte completa sob AddressSanitizer e UndefinedBehaviorSanitizer, e passar clang-tidy e cppcheck sem avisos.
6. Revisão de segurança conforme a seção 17: sem mídia sem criptografia, sem credenciais em texto puro, tokens protegidos, TURN com credenciais efêmeras.
7. Crash reporting.

Critérios de aceitação:

- Todas as métricas da seção 22 medidas e registradas em `docs/benchmarks.md`.
- A chamada sobrevive a 5% de perda de pacotes com degradação suave e sem queda.
- Nenhum achado aberto de alta severidade na revisão de segurança.

---

### M9 — Empacotamento e release

Objetivo: a seção 25 da SPEC.

Tarefas:

1. Instalador Windows, com as DLLs do Qt e do runtime da Microsoft.
2. AppImage no Linux.
3. Bundle `.app` e imagem `.dmg` no macOS, com notarização.
4. Assinatura de código no Windows e no macOS.
5. Publicação automática de artefatos por tag de release.
6. `docs/build.md` e `docs/release.md`.

Critérios de aceitação:

- Uma tag gera os quatro artefatos automaticamente.
- Cada artefato instala e roda em uma máquina limpa da respectiva plataforma.

---

## 4. Rastreamento dos critérios de aceitação do MVP

Mapeamento dos 15 critérios da seção 29 da SPEC para os marcos que os cobrem.

| Critério da SPEC | Marco |
| --- | --- |
| 1, 2. Criar e entrar em sala | M2, M7 |
| 3. Cinco usuários simultâneos | M5 |
| 4, 5. Áudio com baixa latência | M4, M5 |
| 6, 7, 8. Compartilhamento em 720p a 30 FPS | M6 |
| 9, 10, 11. H.264, Opus, WebRTC | M4, M6 |
| 12. Três plataformas | M0, M3 |
| 13. Builds automatizados | M0, M9 |
| 14. Aplicativo responsivo | M6, M7 |
| 15. Sem dependência de uma única plataforma | M6 |

---

## 5. Riscos e mitigações

| Risco | Impacto | Mitigação |
| --- | --- | --- |
| Conflito entre a libc++ do Chromium e o libstdc++ do Qt | Baixo no Linux | Resolvido: o build do fonte com `use_custom_libcxx=false` foi concluído e verificado. Windows e macOS ainda não confirmados. |
| Manter um build próprio da libwebrtc para 3 plataformas | Médio | Consequência da decisão acima. `scripts/build_webrtc.sh` automatiza, mas alguém precisa hospedar os binários e refazer a cada atualização de milestone. O build depende de uma libstdc++ 14 fixada, porque a faixa de versões que o clang do Chromium aceita é estreita. |
| A libwebrtc não compila ou não linka no Windows ou no macOS | Alto | Linux já validado. Os outros dois rodam pelo job `webrtc-spike` do CI. |
| Interoperação entre libwebrtc no cliente e libdatachannel no SFU | Retirado | Validado no M4 por teste de integração: negociação, ICE, DTLS e áudio entre dois clientes libwebrtc através do SFU. |
| Ciclo de vida do dispositivo de áudio no Linux | Retirado | Era um bug da libwebrtc, corrigido por patch do projeto. Detalhado no M4. |
| Captura em Wayland exige portal e consentimento do usuário | Médio | A interface `ScreenCapturer` prevê o fluxo de permissão desde o M6. |
| Encoding H.264 em software estourando o orçamento de CPU | Médio | 720p a 30 FPS é viável em software; encoders por hardware ficam no M8, atrás de interface. |
| Licenciamento e patentes de H.264 | Médio | Levantar antes do M6, e manter VP9 como plano B já previsto na arquitetura. |
| Escopo do SFU crescendo (simulcast, BWE) | Médio | Bitrate fixo no MVP, adaptação só no M8. |
| Contas de desenvolvimento com senha em texto puro no `users_file` | Médio | Aceito só no MVP. O hash já é salgado em memória, mas precisa virar Argon2id e um user store de verdade antes de qualquer deploy. |

---

## 6. Sequência sugerida

M0 e M1 podem ser feitos em paralelo com o M3, porque não dependem da libwebrtc.
O M2 depende do M1.
O M4 depende do M2 e do M3, e é o ponto de decisão do projeto.
De M5 em diante a sequência é linear.

Estado atual: M0, M1 e M2 concluídos e verificados.
O M3 está validado no Linux sobre a biblioteca construída do fonte, incluindo captura de tela real em X11.
Falta a captura em Wayland, e rodar o spike no Windows e no macOS.

O M4 está entregue, menos a medição de latência boca a ouvido, que exige duas máquinas.
Signaling, SFU, mídia do cliente, métricas e UI provisória existem, são testados de ponta a ponta com libwebrtc de um lado e libdatachannel do outro, e funcionam na aplicação real.

O M5 está entregue.
Dispositivos, volume por participante, níveis, detecção de fala e o processamento de áudio da seção 9 estão implementados e verificados com áudio real, sobre um dispositivo virtual que também roda no CI.
Falta apenas a parte do primeiro critério que exige cinco pessoas em cinco máquinas para julgar eco.
O próximo marco é o M6, compartilhamento de tela.
