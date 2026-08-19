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
