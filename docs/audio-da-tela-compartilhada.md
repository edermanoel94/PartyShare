# Áudio da tela compartilhada

Hoje o PartyShare envia a tela e o microfone. O que falta é o som que sai da
máquina de quem compartilha: a pessoa abre o Chrome, dá play num vídeo, e do
outro lado só se ouve a voz dela.

Este documento é o levantamento de como isso entraria, e o plano.

## 1. O que existe hoje

| Peça | Onde | O que faz |
| --- | --- | --- |
| Captura de tela | `client/src/video/screen_capturer.hpp` | Um monitor inteiro, via `DesktopCapturer` do libwebrtc. Não há captura por janela. |
| Microfone | `client/src/webrtc/libwebrtc_media_session.cpp:610` | Uma `AudioTrack` por sessão, sobre o `AudioDeviceModule` da plataforma. |
| Processamento | mesmo arquivo, linhas 324-344 | AEC3, supressão de ruído em `kHigh`, AGC1 adaptativo, pipeline mono a 48 kHz. |
| Transporte | `server/src/sfu/media_router.cpp` | O servidor sempre oferta. Cada participante tem 1 m-line de áudio de subida, 1 de vídeo de subida, 1 de vídeo de descida e N-1 de áudio de descida. |
| Perfil Opus na oferta | libdatachannel, `DEFAULT_OPUS_AUDIO_PROFILE` | Já é `stereo=1;sprop-stereo=1;maxaveragebitrate=96000;useinbandfec=1`. |

Duas coisas dessa tabela mudam o desenho inteiro e vale destacá-las:

**A oferta já é estéreo e já permite 96 kbps.** `media_router.cpp:188` chama
`addOpusCodec(payload_type)` sem segundo argumento, então herda o perfil padrão
do libdatachannel, que anuncia `stereo=1` e `maxaveragebitrate=96000`. O caminho
de rede para música já está negociado — só não há música entrando nele.

**Só uma pessoa compartilha por vez**, garantido pelo `RoomManager`. Então o
áudio da tela pode viajar na trilha de quem compartilha, sem trilha nova.

## 2. As três perguntas

O problema se separa em três, e cada uma tem uma resposta independente:

1. **Como capturar** o som de um aplicativo no Windows.
2. **Como fazer esse PCM entrar no libwebrtc**, que só conhece um dispositivo de
   captura.
3. **Como levar até os outros participantes.**

A ordem de dificuldade é 2 > 1 > 3. A terceira é quase de graça.

## 3. Captura — WASAPI process loopback

O Windows 10 build 20348 e superiores têm captura de loopback **por processo**,
que é exatamente o caso do pedido: capturar o que o Chrome toca, e nada mais.

```cpp
AUDIOCLIENT_ACTIVATION_PARAMS params{};
params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
params.ProcessLoopbackParams.TargetProcessId = pid_do_chrome;
params.ProcessLoopbackParams.ProcessLoopbackMode =
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

PROPVARIANT activation{};
activation.vt = VT_BLOB;
activation.blob.cbSize = sizeof(params);
activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                            __uuidof(IAudioClient), &activation,
                            handler, &operation);
```

Cabeçalho `audioclientactivationparams.h`, e a ativação é assíncrona — a
interface chega por `IActivateAudioInterfaceCompletionHandler`, que precisa
implementar `IAgileObject` para não travar quando o Windows chama de volta do
MTA.

Dois modos importam:

- `INCLUDE_TARGET_PROCESS_TREE` com o PID do app escolhido: só aquele app.
- `EXCLUDE_TARGET_PROCESS_TREE` com `GetCurrentProcessId()`: **todo o sistema
  menos o próprio PartyShare**. Esse é o modo "compartilhar o áudio do sistema",
  e a exclusão não é um detalhe — é o que impede que a voz dos outros
  participantes, que sai pelos alto-falantes, volte para dentro da chamada.

O stream de loopback vem no formato de mixagem do endpoint, tipicamente
`WAVE_FORMAT_IEEE_FLOAT` estéreo a 48 kHz. Precisa virar `int16` intercalado a
48 kHz para o resto do caminho. Um detalhe conhecido do loopback por processo: o
Windows **não gera pacotes de silêncio** quando o app não toca nada, então o
laço de captura precisa preencher silêncio pelo relógio, senão a trilha
"congela" e o receptor escuta o último buffer esticado.

Enumerar os apps que estão tocando som, para o seletor da interface, sai de
`IAudioSessionManager2::GetSessionEnumerator` → `IAudioSessionControl2::GetProcessId`.

## 4. A parte difícil — entrar no libwebrtc

O libwebrtc tem **um** `AudioDeviceModule` por `PeerConnectionFactory`, e o
`AudioTransportImpl` entrega o mesmo frame capturado para todos os
`AudioSendStream`. Duas trilhas locais de áudio na mesma `PeerConnection`
carregam, por construção, o mesmo som. Isso descarta a solução ingênua de "criar
uma segunda `AudioTrack` com o loopback".

Três saídas existem. Verifiquei as três contra os cabeçalhos do dist m152 em
`C:\Users\ederc\.cache\partyshare\webrtc\dist\include`.

### Opção A — `AudioFrameProcessor` (recomendada)

`api/audio/audio_frame_processor.h` define um gancho oficial, e
`api/peer_connection_interface.h:1481` confirma que
`PeerConnectionFactoryDependencies` tem o campo `audio_frame_processor`. O
comentário do cabeçalho é explícito: *"will be used for additional processing of
captured audio frames, performed before encoding"*, chamado **fora do caminho de
tempo real da captura**.

Isso significa que ele roda **depois do APM e antes do codificador** — medido na
fase 0, não deduzido: um tom de 20 Hz injetado a 0,354 de amplitude chega ao
processador a 0,0001, porque o filtro passa-alta já passou por ele. É o lugar
certo: o AEC3, a supressão de ruído e o AGC já terminaram com o microfone, e a
música entra sem passar por nenhum deles. Misturar antes do APM seria o
contrário — o supressor de ruído em `kHigh` destrói música, e o AGC bombeia o
volume a cada nota.

```cpp
class ScreenAudioMixer : public webrtc::AudioFrameProcessor {
  void Process(std::unique_ptr<webrtc::AudioFrame> frame) override {
    // frame é o microfone, mono, 48 kHz, já processado.
    // Vira estéreo; a música entra por cima.
    mix_loopback_into(*frame);
    sink_(std::move(frame));
  }
  void SetSink(OnAudioFrameCallback sink) override { sink_ = std::move(sink); }
};
```

`AudioFrame::mutable_data(samples_per_channel, num_channels)` aceita trocar a
contagem de canais, e o `ChannelSend` remixa para o formato do codificador. Como
a oferta já negociou `stereo=1`, o Opus do lado de quem compartilha já está
configurado para dois canais — passar frames realmente estéreo transforma um
estéreo falso (mono duplicado) em estéreo de verdade, sem tocar em SDP.

**Custo:** uma classe nova, um campo a mais em `dependencies`, zero mudança no
servidor, zero renegociação.

**O que ela não dá:** o som da tela viaja dentro da trilha de voz. As
consequências disso estão na seção 6.

### Opção B — segunda `PeerConnection` só para o áudio da tela

Uma `PeerConnectionFactory` própria, com um ADM próprio alimentado pelo
loopback, uma trilha `sendonly` até o SFU, APM desligado, Opus estéreo em 128
kbps. Do lado do servidor, uma m-line `sfu-screen-audio` de descida por sessão,
espelhando exatamente o que `add_video_outbound_track` já faz para o vídeo — os
espectadores recebem como mais uma trilha remota e nem precisam saber que é
diferente.

**A favor:** separação real. Mute do microfone independente do volume do filme,
volume por fonte, qualidade de música sem compromisso.

**Contra:** um segundo ICE e um segundo DTLS por quem compartilha, o que
contraria a decisão registrada em `media_router.hpp` ("both travel on the same
connection per participant, because a second one would be a second ICE
negotiation and a second DTLS handshake for no gain"). E, mais sério: dois
transportes separados não têm grupo de sincronização RTP, então **não há
garantia de lip sync** entre o vídeo da tela e o som dele. Para um vídeo do
YouTube com alguém falando, isso é o defeito que se nota.

### Opção C — `CreateAudioDeviceWithDataObserver`

`modules/audio_device/include/audio_device_data_observer.h` existe no dist, mas
`OnCaptureData` recebe `const void*`. Escrever ali exige `const_cast` sobre um
buffer de terceiros, e a mistura aconteceria **antes** do APM. Descartada: é o
pior dos dois mundos.

### Decisão

**Opção A para a primeira versão, confirmada pela fase 0.** Ela entrega o pedido — ouvir o YouTube de
quem compartilha — com o menor risco e sem tocar no servidor. A Opção B fica
registrada como o caminho de evolução, se e quando o controle separado de volume
passar a importar mais do que o lip sync.

## 5. Transporte — o que muda no servidor

Na Opção A: **nada**. A trilha de áudio de quem compartilha já existe, já é
encaminhada para todos, e já está negociada em estéreo a 96 kbps.

Duas coisas valem revisar de qualquer forma:

- `AudioConfig::bitrate_kbps` (`shared/include/dv/config/config.hpp:35`) é lido,
  validado entre 6 e 510, e **nunca aplicado** em lugar nenhum do caminho de
  mídia. Com música na trilha ele finalmente teria razão de existir: passa a ser
  o `maxaveragebitrate` da oferta, via o segundo argumento de `addOpusCodec`.
- `docs/requirements.md:136` estima 200 kbps por participante para áudio. Com
  estéreo a 96 kbps na trilha de quem compartilha, essa conta sobe. Vale
  atualizar o número junto com a mudança.

## 6. As consequências que o desenho arrasta

Misturar numa trilha só é barato, mas não é neutro. Quatro coisas quebram se
forem ignoradas, e as quatro têm conserto no mesmo lugar:

**Mudo do microfone mataria o filme.** `set_microphone_muted` faz
`local_track_->set_enabled(false)` (`libwebrtc_media_session.cpp:683`), o que
desliga a trilha inteira. Conserto: manter a trilha ligada e zerar **só a parte
do microfone** dentro do `ScreenAudioMixer`. Fica melhor do que hoje, inclusive:
o mudo passa a ser exato em vez de depender do estado da trilha.

**Baixar o volume de quem compartilha mataria o filme.**
`set_participant_volume` age sobre a trilha remota, que agora carrega as duas
coisas. Sem conserto dentro da Opção A — é o preço dela, e precisa estar escrito
na interface ("volume de <fulano>" passa a incluir o que ele compartilha).

**O indicador de fala ficaria aceso o tempo todo.** O nível local sai das
estatísticas de `outbound-rtp` (`collect_levels`, linha 1227), que são pós
mistura; o nível remoto sai da extensão de cabeçalho `audio-level`, que também
é. Conserto: medir o nível do microfone dentro do `ScreenAudioMixer`, antes de
misturar, e publicar esse valor. Para os espectadores, o nível remoto de quem
compartilha passa a não significar nada — a interface deve deixar de mostrar o
indicador de fala de quem está compartilhando, ou mostrá-lo a partir de outra
fonte.

**Realimentação.** Se o loopback capturar a saída do próprio PartyShare, a voz
de todo mundo volta para dentro da chamada, depois do AEC, sem nada para
cancelá-la. É o modo de falha mais grave e o único que estraga a chamada para
todos. Por isso o modo do loopback nunca é "tudo": ou é `INCLUDE` de um app
escolhido, ou é `EXCLUDE` do próprio processo.

## 7. Interface, configuração e protocolo

**Captura é de monitor, não de janela.** O pedido fala em "compartilhando o
Chrome", mas hoje o PartyShare compartilha um monitor inteiro. Então a escolha
do app é uma escolha de *áudio*, separada da escolha do que aparece na tela. A
interface precisa dizer isso sem mentir.

No diálogo de compartilhar (`main_window.cpp:924`, `on_toggle_share`):

```
Compartilhar tela
  Monitor:  [ Monitor 1 ▾ ]
  Áudio:    ( ) Nenhum
            (•) Todo o sistema (menos o PartyShare)
            ( ) Apenas um aplicativo:  [ Chrome ▾ ]
```

Configuração, em `AudioConfig` ou numa seção nova:

```ini
[screen_audio]
mode = system        ; none | system | process
bitrate_kbps = 96
```

Protocolo: `ScreenShareStarted` (`shared/include/dv/protocol/message.hpp:186`)
ganha um `bool has_audio`. Não é necessário para o som chegar — ele chega pela
trilha de voz de qualquer jeito — mas é o que permite a interface mostrar um
ícone de alto-falante junto de "fulano está compartilhando", e explicar por que
o volume daquela pessoa agora controla duas coisas.

## 8. Plano

### Fase 0 — spike ✅ feito

`tools/screen_audio_spike/`, no molde de `tools/webrtc_spike`. Duas
`PeerConnection` no mesmo processo, ligadas por candidatos de host, nenhuma
placa de som envolvida: as duas pontas usam um `AudioDeviceModule` próprio sobre
`AudioDeviceModuleDefault`. O emissor injeta um tom de 20 Hz; o processador
descarta o que recebe e escreve 440 Hz à esquerda e 1000 Hz à direita; o
receptor puxa dois canais e mede a energia em cada frequência, em cada canal.

```
cmake --preset windows-release -DDV_ENABLE_SCREEN_AUDIO_SPIKE=ON \
  -DDV_WEBRTC_ROOT=<dist> -DDV_BUILD_CLIENT=OFF -DDV_BUILD_SERVER=OFF -DDV_BUILD_TESTS=OFF
cmake --build build/windows-release --target dv_screen_audio_spike
build/windows-release/bin/screen-audio-spike.exe
```

Resultado, idêntico em quatro execuções:

| Verificação | Resultado |
| --- | --- |
| `audio_frame_processor` é chamado | sim, 652 frames em 652 blocos injetados |
| roda depois do APM | sim, senha 0 para o APM e 1 para o processador |
| o tom de 20 Hz sobrevive até ele? | não: 0,354 injetado vira 0,0001 — o passa-alta já passou |
| Opus negociado no emissor | `opus, 2 canais, pt 111` |
| o áudio do processador chega | sim |
| chega estéreo | sim: esquerda 440=0,3003 / 1000=0,0000; direita 440=0,0001 / 1000=0,3000 |

Os 0,3003 e 0,3000 são a amplitude que o processador escreveu, 0,30, chegando
intacta e no canal certo. As três afirmações da seção 4 estão medidas.

Duas coisas que o spike ensinou de quebra:

- **O emissor codifica em estéreo por causa do `stereo=1` da oferta, não da
  resposta.** A resposta que o libwebrtc escreve não traz `stereo=1` no `fmtp`,
  e mesmo assim `GetParameters()` reporta Opus com dois canais. O `fmtp` da
  resposta descreve o que aquele lado aceita *receber*, e a m-line é `sendonly`
  para ele. Quem manda no que ele codifica é a oferta — que, em produção, é a do
  SFU, e ela já diz `stereo=1`. Ou seja: nada a mudar no servidor, confirmado
  por medida e não por leitura de SDP.
- **`InitializeSSL()` não abre o Winsock.** Um executável nu que só chame
  `InitializeSSL` vê todo socket de ICE falhar com `WSANOTINITIALISED`; o spike
  chama `WSAStartup` por conta própria. O cliente não sofre disso porque o Qt já
  abriu o Winsock antes — mas qualquer teste de integração novo que suba a
  camada de mídia fora do Qt vai precisar da mesma linha.

### Fase 1 — captura no Windows ✅ feito

Segue o padrão de `screen_capturer.hpp`: interface pura no cabeçalho,
implementação por plataforma. Fica em `dv_client_core`, **não** atrás de
`DV_BUILD_CLIENT_MEDIA` — é API de plataforma e não precisa de libwebrtc, então
um build sem a camada de mídia enxerga a captura normalmente.

- `client/src/audio/loopback_capturer.hpp` — `start(mode, pid)`, `stop()`, um
  `BlockSink` de PCM `int16` estéreo a 48 kHz, um `ErrorSink`, e
  `LoopbackStats`. Dois modos: `System` (tudo menos este processo) e `Process`
  (uma árvore de processos).
- `client/src/audio/block_pacer.hpp` — o amortecedor entre o relógio do WASAPI e
  o relógio de 10 ms. É onde a deriva vira decisão: acima da marca d'água
  descarta o mais antigo, embaixo entrega silêncio, e depois de uma fome espera
  reencher em vez de alternar áudio e silêncio a cada 10 ms.
- `client/src/audio/loopback_capturer_windows.cpp` — a ativação assíncrona, o
  `IActivateAudioInterfaceCompletionHandler` com `IAgileObject`, e o laço de
  captura.
- `client/src/audio/audio_sources.hpp` + `audio_sources_windows.cpp` — os apps
  com sessão de áudio, via `IAudioSessionManager2`, com este processo sempre
  fora da lista.
- `client/src/audio/loopback_stub.cpp` — `capture_unavailable` fora do Windows.

**Não há conversão de formato.** O plano previa converter float32 para int16;
não foi preciso. O loopback por processo deixa o formato ser escolhido por quem
captura, e com `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` o motor de áudio reamostra e
remixa para o que se pedir. Pedimos 48 kHz, 2 canais, 16 bits — o formato que o
resto do caminho já fala — e não sobra conversão nenhuma para escrever.

**Nada de Media Foundation.** O exemplo da Microsoft usa filas de trabalho do MF
(`MFStartup`, `MFPutWaitingWorkItem`). Uma thread esperando num evento faz o
mesmo, e a dependência não se paga.

#### Testes

`tests/unit/test_block_pacer.cpp`, nove testes sem plataforma nenhuma: ordem,
prime, fome, marca d'água, silêncio sem buffer de zeros, pacotes que não são
múltiplos de um bloco.

`tests/integration/test_loopback_capture.cpp` contra o sistema de verdade. O
teste renderiza um tom de 440 Hz a 0,05 de amplitude do próprio processo e mede
por correlação — não por volume — quanto desse tom chegou:

| Teste | O que prova |
| --- | --- |
| `ItHearsWhatAProcessIsPlaying` | modo `Process` apontado para nós ouve o tom. É a funcionalidade. |
| `SystemModeIsDeafToThisProcess` | modo `System` **não** ouve o tom. É a proteção contra realimentação da seção 6. |
| `BlocksArriveOnTheClock...` | 80 a 120 blocos por segundo, todos de 10 ms exatos, nenhum descartado. |
| `AProcessLoopbackNeedsAProcess` | pid zero em modo `Process` é `invalid_value`. |
| `TheListNeverContainsThisProcess` | a enumeração nunca oferece o próprio PartyShare. |

Correlação em vez de volume porque a máquina que roda o teste bem pode ter um
navegador tocando: "chegou algum som" não distingue isso de "chegou o nosso
tom", e 440 Hz distingue. Na prática isso importou — a máquina de
desenvolvimento tinha o Chrome tocando durante a validação, e o teste de surdez
passou mesmo assim.

#### Dois defeitos que a fase 1 encontrou em si mesma

**COM fechado antes das interfaces.** A `Session` com o `IAudioClient` e o
`IAudioCaptureClient` era destruída depois do `CoUninitialize` da thread de
captura, o que libera interfaces num apartamento que já não existe. O sintoma
era `0xC0000005` alguns milissegundos depois de qualquer `stop()`, e sumia com
qualquer instrumentação que mudasse o tempo. O escopo que resolve isso está
comentado no arquivo, porque é o tipo de linha que alguém "arruma" depois.

**`audio_sources()` exigia COM já inicializado.** Um teste, uma ferramenta ou
uma thread de trabalho não inicializam, e o enumerador falhava com
`CO_E_NOTINITIALIZED` — que se lê exatamente como "esta máquina não tem placa de
som". Agora a função inicializa se ninguém tiver inicializado, e desfaz o que
fez.

### Fase 2 — mistura ✅ feito

- `client/src/audio/screen_audio_mixer.hpp/.cpp` — a aritmética, sem nenhum tipo
  do libwebrtc dentro. `mix()` recebe o microfone e devolve microfone mais tela,
  com ganho, saturação e contagem de canais. Reusa o `BlockPacer` da fase 1 como
  a fila de ressincronização entre o relógio da captura e o que o libwebrtc puxa.
- `client/src/webrtc/screen_audio_frame_processor.hpp` — o `AudioFrameProcessor`
  propriamente dito, fino de propósito: copia o frame, chama `mix()`, escreve o
  resultado de volta com a contagem de canais que voltou.
- `libwebrtc_media_session.cpp` — o misturador vive na `Engine`, um por
  processo, pelo mesmo motivo que a fonte do microfone: ele é pendurado na
  factory, e a factory é do processo. Instalado sempre, compartilhando alguém ou
  não — sem nada para misturar ele copia o frame adiante, e instalá-lo depois
  significaria reconstruir a factory no meio da chamada.
- `media_session.hpp` — `start_screen_audio(mode, source_id)`,
  `stop_screen_audio()`, `screen_audio_active()`, e `AudioStats` com
  `microphone_level`, `screen_audio_active` e a contagem de blocos.

#### Três coisas que mudaram de mecanismo

**O mudo.** Era `local_track_->set_enabled(false)`. Agora é isso *só quando não
há compartilhamento de áudio*; com um em curso a trilha fica ligada e o
microfone é zerado dentro do misturador, senão desligar a trilha levaria o filme
junto. `apply_track_enabled()` é o único lugar que decide, e é chamado pelo
mudo, pelo início e pelo fim do compartilhamento.

**O indicador de fala.** Era o `audio_level` de `outbound-rtp`, que é medido
depois da mistura — com um filme na trilha, mostraria a pessoa falando o tempo
todo. Agora vem do misturador, que mede o microfone sozinho, antes do ganho do
mudo e antes de somar qualquer coisa. Ficou mais exato mesmo sem
compartilhamento nenhum: é o microfone de verdade, não o que sobrou depois de
codificar.

**O estéreo é condicional.** Sem compartilhamento a trilha continua mono; pedir
ao codificador dois canais idênticos numa chamada só de voz seria pagar por
nada. Com um em curso a saída vira estéreo e *fica* estéreo mesmo enquanto a
captura passa fome, para a contagem de canais não oscilar toda vez que o
aplicativo fica quieto.

#### Verificação

Catorze testes de unidade em `tests/unit/test_screen_audio_mixer.cpp`: ordem dos
canais, voz nos dois ouvidos, saturação nas duas pontas da escala, mudo com e
sem compartilhamento, fome da captura, frame que não é um bloco, e o que
acontece depois de `stop()`.

E o spike ganhou um segundo modo, `screen-audio-spike mixer`, que troca o
processador escrito lá pelo **misturador e pelo adaptador do produto** e empurra
o áudio de tela pela mesma costura que a captura do Windows usa. Prova a fase 2
inteira sobre uma conexão WebRTC de verdade:

```
[ .. ] energia recebida   esquerda 440=0.3001 1000=0.0000 voz=0.1997
                          direita  440=0.0001 1000=0.2978 voz=0.1996
[ OK ] 1. o misturador do produto roda     nivel do microfone: 0.1414
[ OK ] 2. a voz chega nos dois canais      300 Hz: esquerda 0.1997, direita 0.1996
[ OK ] 3. o audio da tela chega em estereo 440 esq. 0.3001, 1000 dir. 0.2978
```

0,1414 é exatamente 0,2/√2, o valor eficaz do tom injetado no microfone. A voz
sai nos dois canais com a amplitude com que entrou, o áudio de tela chega cada
tom no seu canal, e o vazamento cruzado é da ordem de 10⁻⁴.

Sem WASAPI nesse modo, de propósito: que a captura ouve o que um processo toca
já está medido na fase 1. O que faltava medir era o que acontece com esses
blocos depois.

#### Um obstáculo que não é desta feature

`dv_media_tests` **não linka nesta máquina**, e nunca linkou: `webrtc.lib` traz
BoringSSL dentro e o `libssl` do vcpkg define os mesmos símbolos, então o
linkador recusa com uma parede de `LNK2005`. Só o `.pdb` do alvo existe em
`build/media/bin`. Por isso a verificação da fase 2 foi pelo spike, que linka só
o libwebrtc e o `dv_shared` — e por isso o teste ponta a ponta da fase 4 vai
precisar que esse conflito seja resolvido antes.

### Fase 3 — sessão, interface e configuração ✅ feito

**A sessão.** `CallSession::start_screen_share(monitor, ScreenAudio)` liga o som
depois que a captura da tela pegou, e só então anuncia — para que o `has_audio`
do anúncio seja verdade e não intenção. `stop_screen_share` desliga o som
primeiro, e `on_screen_share_ended` já chamava `stop_screen_share`, então uma
captura que morre sozinha leva o áudio junto sem código novo.

Uma decisão que vale escrever: **o som não derruba o compartilhamento**. Quem
pediu para compartilhar um vídeo e ouviu "não" porque este Windows é um ano
velho demais prefere a imagem a nada. Então a tela sobe, `screen_audio_active()`
diz o que de fato aconteceu, `screen_audio_failure()` diz por quê, e a barra de
status escreve "Sharing without sound: ...". Um compartilhamento mudo que era
para ter som é, de outro modo, indistinguível de um que nunca ia ter.

**A interface.** O seletor não foi para um diálogo de compartilhar, porque não
existe um: o botão alterna direto e o monitor é escolhido em Configurações. O
som foi para o mesmo lugar que o monitor — duas linhas novas no formulário de
vídeo, "Share sound" (None / Everything but PartyShare / One application) e
"Application", mais uma linha de dica que explica um controle desabilitado em
vez de deixá-lo morto. A lista de aplicativos é relida a cada troca de modo, não
uma vez ao abrir: ela é do que está tocando *agora*.

Na lista de participantes, quem compartilha com som aparece como
`(sharing with sound)` em vez de `(sharing)`. São duas perguntas diferentes —
de quem é a imagem na tela, e por que o volume daquela pessoa agora é também o
volume de um filme.

**O protocolo.** `ScreenShareStarted` ganhou `has_audio`, e o servidor o lembra
em `models::Participant::sharing_audio` para contar a quem entra no meio. Lido
com *fallback* falso: um par construído antes disso não manda o campo, e "não
manda" quer dizer "sem som" — não "mensagem malformada", que faria o
compartilhamento de um cliente antigo deixar de ser anunciado.

**A configuração.** Seção `[screen_audio]` com `mode`, validada contra
`none|system|process`. O padrão é `system`, porque compartilhar um vídeo e
ninguém ouvir é que é a surpresa; e não é um padrão silencioso, já que o diálogo
mostra a escolha antes de qualquer compartilhamento. Um modo que este build não
conhece lê como `none` — cair para capturar a máquina porque uma palavra não foi
entendida é a única resposta inaceitável.

E `audio.bitrate_kbps` finalmente chega à oferta, via
`MediaRouter::Options::opus_max_bitrate_kbps`. **O padrão subiu de 48 para 96**,
e isso não é mudança de comportamento: o que trafegava era o padrão do
libdatachannel, 96. Deixá-lo em 48 ao ligá-lo pela primeira vez cortaria pela
metade o som de todo compartilhamento. Agora o número descreve o que já era
verdade.

Onze testes novos: ida e volta do `has_audio`, a mensagem sem o campo, o
servidor contando a quem entra no meio, o campo esquecido ao parar, a seção nova
do INI, o modo inválido recusado, e o mapeamento de modos nos dois sentidos.

#### O que não deu para verificar rodando

**O cliente Qt com a camada de mídia não linka nesta máquina**, pelo mesmo
conflito BoringSSL/OpenSSL da fase 2. Não é regressão: `build/windows-release`
tem a interface ligada e a mídia desligada, `build/media` o contrário, e
`partyshare.exe` com as duas nunca existiu aqui. Tudo compila nas duas
configurações e a lógica está coberta por testes, mas os dois controles novos do
diálogo não foram vistos na tela.

### Fase 4 — medição e documentação

- `VideoStats`/`AudioStats` ganham o bitrate do áudio da tela, para o painel de
  métricas que já existe.
- `docs/requirements.md` — a conta de banda revisada.
- Um teste ponta a ponta em `tests/integration/`, no molde de
  `test_media_end_to_end.cpp`: um cliente gera um tom, compartilha, e o outro
  recebe áudio acima do piso de ruído. `DV_AUDIO_NULL_DEVICE` já existe para
  rodar isso sem placa de som. **Bloqueado** até o conflito
  BoringSSL/OpenSSL descrito na fase 2 ser resolvido: hoje `dv_media_tests` não
  linka nesta máquina.

## 9. Riscos

| Risco | Peso | O que fazer |
| --- | --- | --- |
| ~~`audio_frame_processor` não ser honrado no dist m152~~ | — | Descartado pela fase 0: é honrado, roda depois do APM, e o estéreo atravessa. |
| Realimentação da própria saída | alto | Nunca capturar loopback sem `EXCLUDE` do próprio PID ou `INCLUDE` de um app. |
| ~~Deriva de relógio entre o loopback e o relógio de captura~~ | — | Resolvido na fase 1 por `audio::BlockPacer`, com marca d'água, descarte do mais antigo e reenchimento depois de uma fome. |
| Windows anterior ao build 20348 | baixo | **Decidido na fase 1: desabilitar, não cair para o loopback de endpoint.** O loopback de endpoint não sabe excluir o próprio processo, e um modo que pode realimentar a chamada é pior do que um modo que não existe. `loopback_capture_is_available()` responde falso e a interface não oferece a opção. |
| Banda: 96 kbps estéreo contra 48 mono | baixo | Já cabe na estimativa de `docs/requirements.md` depois de revista. |

## 10. Fora de escopo

- **Linux e macOS.** No Linux é módulo de loopback do PulseAudio/PipeWire, no
  macOS é `ScreenCaptureKit` com `SCStreamOutputType.audio` (ou um dispositivo
  agregado em versões antigas). São implementações inteiras, não ressalvas — a
  interface `LoopbackCapturer` fica pronta para elas, o `_stub.cpp` responde por
  elas até lá.
- **Captura por janela.** Compartilhar só a janela do Chrome, em vez do monitor,
  é uma mudança do lado do vídeo e não depende deste trabalho.
- **Volume separado por fonte no receptor.** Só existe na Opção B.
