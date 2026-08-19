# Build

## Pré-requisitos

| Ferramenta | Versão mínima | Observação |
| --- | --- | --- |
| CMake | 3.25 | Presets versão 6 |
| Ninja | 1.11 | Gerador padrão de todos os presets |
| Qt | 6.5 | Apenas para o cliente |
| Compilador | MSVC 2022, GCC 12, Clang 15 | C++20 |

Dependências resolvidas automaticamente: spdlog, nlohmann/json e GoogleTest.
Cada uma é procurada primeiro com `find_package`, e só é baixada via `FetchContent` quando não estiver instalada.
Isso permite usar vcpkg, pacotes da distribuição ou nada, sem alterar o CMake.

Duas dependências precisam existir de verdade, porque não são header only:

| Dependência | Usada por | Onde obter |
| --- | --- | --- |
| libdatachannel | Servidor: WebSocket do signaling (M2) e SFU (M4) | vcpkg ou pacote da distribuição |
| OpenSSL | Servidor: hash de senha e tokens | vcpkg ou pacote da distribuição |

Sem elas, use `-DDV_BUILD_SERVER=OFF` para compilar apenas o cliente e os testes compartilhados.

No Linux, quem linka a libwebrtc precisa também dos headers de X11, glib, gbm e libdrm.
Eles não são dependências nossas: vêm da captura de tela da própria libwebrtc, que fala com o portal do XDG por GDBus e importa frames como DMA-BUF.

```sh
# Arch
sudo pacman -S --needed libx11 libxext libxfixes libxdamage libxrandr libxcomposite libxtst glib2 mesa libdrm

# Debian e Ubuntu
sudo apt install libx11-dev libxext-dev libxfixes-dev libxdamage-dev libxrandr-dev \
  libxcomposite-dev libxtst-dev libglib2.0-dev libgbm-dev libdrm-dev
```

Com vcpkg:

```sh
cmake --preset linux-release -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

## Camada de mídia do cliente

O cliente compila sem a libwebrtc por padrão.
Nesse modo tudo funciona menos o áudio: `create_media_session` falha com `media_unavailable`, e a interface e o signaling continuam inteiros.

Para compilar com mídia é preciso a árvore que o `scripts/build_webrtc.sh` produz, pelos motivos da seção 5 de [webrtc-toolchain.md](webrtc-toolchain.md):

```sh
cmake -S . -B build/media \
  -DDV_BUILD_CLIENT_MEDIA=ON \
  -DDV_WEBRTC_ROOT=$HOME/.cache/desktop-voice/webrtc/dist
cmake --build build/media
```

Duas variáveis de ambiente ajudam a depurar mídia:

| Variável | Efeito |
| --- | --- |
| `DV_WEBRTC_LOG` | `warning`, `info` ou `verbose`. Liga o log interno da libwebrtc, que é a única forma de ver por que um dispositivo não abriu ou um codec foi recusado. |
| `DV_AUDIO_NULL_DEVICE` | Usa um dispositivo de áudio nulo em vez do sistema. Serve para máquina sem placa de som e para CI. Nada é capturado nem reproduzido. |
| `DV_VIRTUAL_INPUT_DEVICE` | Nome do dispositivo de captura que os testes de mídia devem usar. Exportado pelo `scripts/virtual_audio.sh`, descrito abaixo. |
| `DV_VIRTUAL_OUTPUT_DEVICE` | O mesmo para a reprodução. |
| `DV_DUMP_SDP` | Faz o SFU registrar cada oferta que envia. É a forma de responder "isto foi negociado?" sobre codecs, extensões e feedback. |
| `DV_DISABLE_HARDWARE_ENCODER` | Força o codificador de software mesmo em uma máquina com placa capaz. Serve para comparar os dois e para contornar driver problemático. |
| `DV_CRASH_DIRECTORY` | Onde os relatórios de crash são escritos. O padrão é o diretório de estado da plataforma. |
| `DV_CRASH_REPORTS` | `0` desliga o relatório de crash por completo. |

### Dispositivo de áudio virtual

`DV_AUDIO_NULL_DEVICE` faz os testes de negociação passarem em uma máquina sem placa de som, mas um dispositivo nulo não captura nada.
Tudo que dependa de áudio real, que é a maior parte do M5, continua sem poder ser verificado.

O `scripts/virtual_audio.sh` resolve isso criando uma placa de som virtual em cima do PulseAudio, com um tom tocando no microfone:

```sh
eval "$(scripts/virtual_audio.sh start)"
ctest --test-dir build/media -L media --output-on-failure
scripts/virtual_audio.sh stop
```

O `eval` exporta `DV_VIRTUAL_INPUT_DEVICE` e `DV_VIRTUAL_OUTPUT_DEVICE`, e os testes de mídia passam a escolher esses dispositivos explicitamente.

Em uma máquina que já tem servidor de som o script se conecta ao que existe e não mexe nos dispositivos padrão, então rodar os testes não toma a caixa de som de quem está no teclado.
Sem servidor de som algum, que é o caso de um runner de CI, ele sobe um servidor privado e aí sim torna os dispositivos virtuais os padrão.

O microfone virtual é um `module-remap-source` sobre o monitor de um sink nulo, e não o monitor direto: o backend PulseAudio da libwebrtc ignora toda fonte que monitora um sink quando enumera dispositivos de captura, e um dispositivo que ela não lista é um dispositivo que ela não abre.

### Rede degradada

Os testes de mídia degradam a rede por dentro, pelos sockets do próprio cliente, e por isso rodam sem privilégio e sem preparo nenhum.
Para degradar a rede de verdade, nas filas do sistema operacional, existe `scripts/netem.sh`:

```sh
sudo scripts/netem.sh apply lossy      # 5% de perda, o número da seção 22
sudo scripts/netem.sh apply distant    # 150 ms de latência com jitter
sudo scripts/netem.sh apply awful      # os dois, mais reordenação
sudo scripts/netem.sh clear
```

Precisa de root e do módulo `sch_netem`, que não carrega em uma máquina cujo kernel foi atualizado sem reiniciar.
O script diz isso em vez de deixar o `tc` responder que a qdisc é desconhecida.
`DV_NETEM_DRY_RUN=1` mostra o comando que seria executado, sem executá-lo e sem root.

A diferença entre os dois caminhos, e os números medidos com cada um, estão em [benchmarks.md](benchmarks.md).

### Encoder por hardware

O compartilhamento de tela é codificado pela placa quando existe uma, e pelo processador quando não.
No Linux o backend é NVENC, ligado por padrão e desligável com `-DDV_HARDWARE_ENCODER_NVENC=OFF`.

Nada é linkado: `libnvidia-encode.so.1` e `libcuda.so.1` são abertas em tempo de execução, então o mesmo binário roda em uma máquina sem placa NVIDIA.
O cabeçalho da API está em `third_party/nvcodec`, com a procedência ao lado.

Qual codificador está rodando aparece no log a cada intervalo de métricas, lido das estatísticas da libwebrtc:

```text
Video: 1280x720 at 30.0 fps, up 835 kbps, estimate 1621 kbps, 0 frames dropped, encoder OpenH264
```

Quando não há hardware, o motivo é dito uma vez, na criação da engine, e vale a pena ler antes de procurar o problema no driver:

```text
Media: no hardware encoding (the NVIDIA driver does not match its own kernel module, which is
what an upgrade without a reboot leaves behind), the screen is encoded in software
```

`DV_DISABLE_HARDWARE_ENCODER=1` força o software mesmo com placa capaz, que é como se compara os dois.

### Relatórios de crash

Um crash que não deixa nada para trás vira um relato que diz "fechou sozinho".
Cliente e servidor instalam um handler para os sinais em que um crash chega, e escrevem um arquivo com o build, o sinal e o backtrace:

```text
desktop-voice crash report
application: desktop-voice
version: 0.1.0
built: Aug 19 2026 16:36:58

when: 1787168270 seconds since the epoch, readable with: date -d @1787168270
signal: SIGSEGV, a read or write through a bad pointer

backtrace:
./build/media/bin/desktop-voice(+0x145437) [0x5572fb6c8437]
/usr/lib/libQt6Core.so.6(_ZN10QEventLoop4execE...+0x193) [0x7f6888391983]
```

O padrão é `$XDG_STATE_HOME/desktop-voice/crashes` no Linux, `~/Library/Logs` no macOS e `%LOCALAPPDATA%` no Windows, e os dez mais recentes são mantidos.
Cada linha é `binário(+deslocamento) [endereço]`; o deslocamento é o que `addr2line -Cfe <binário> <deslocamento>` transforma em arquivo e linha.
Nomes vindos de bibliotecas saem mangled, porque desfazer isso aloca memória e um handler de sinal não pode: `c++filt` resolve.

O processo continua morrendo como morreria, então core dump configurado continua sendo gerado e o código de saída continua dizendo o que matou o programa.

## Compilar

```sh
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

Presets disponíveis:

```text
linux-debug     linux-release     linux-asan     linux-make
windows-debug   windows-release   windows-asan
macos-arm64-debug   macos-arm64-release   macos-arm64-asan   macos-x64-release
```

O preset `linux-make` usa Unix Makefiles, para máquinas sem Ninja.

Os binários ficam em `build/<preset>/bin/`.

## Opções

| Opção | Padrão | Efeito |
| --- | --- | --- |
| `DV_BUILD_CLIENT` | ON | Compila o cliente. Exige Qt 6. |
| `DV_BUILD_SERVER` | ON | Compila o servidor. |
| `DV_BUILD_TESTS` | ON | Compila a suíte de testes. |
| `DV_ENABLE_SANITIZERS` | OFF | AddressSanitizer e UndefinedBehaviorSanitizer. |
| `DV_WARNINGS_AS_ERRORS` | OFF | Ligado em todos os presets. |
| `DV_ENABLE_WEBRTC_SPIKE` | OFF | Spike do M3. Ver docs/webrtc-toolchain.md. |

Os testes são rotulados: `ctest -L unit` roda só os unitários, `ctest -L integration` só os de integração.
Os de integração sobem um servidor real em porta efêmera e conectam clientes WebSocket de verdade.

Sem Qt instalado, use `-DDV_BUILD_CLIENT=OFF` para compilar apenas o servidor e os testes.

## Configuração em tempo de execução

A precedência é: padrões embutidos, depois arquivo, depois variáveis de ambiente, depois linha de comando.

```sh
./build/linux-release/bin/desktop-voice-server \
  --config=config.json --port=8080 --log-level=debug \
  --users-file=dev-users.json
```

`--users-file` aponta para uma lista de contas de desenvolvimento:

```json
[
  {"username": "ana", "password": "senha-de-teste", "display_name": "Ana"}
]
```

Esse arquivo guarda senhas em texto puro e existe apenas para o MVP ter usuários.
A seção 17 da SPEC proíbe isso em produção, e o servidor loga um aviso a cada inicialização.

Variáveis de ambiente usam o prefixo `DV_`, por exemplo `DV_SIGNALING_URL`, `DV_LOG_LEVEL`, `DV_VIDEO_FPS`.
A lista completa está em `shared/src/config/config.cpp`.

## Formatação e análise estática

```sh
find shared client server tests tools -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
clang-tidy -p build/linux-debug $(find shared server -name '*.cpp')
```

O CI roda ambos, mais cppcheck, mais a suíte sob ASan e UBSan.
