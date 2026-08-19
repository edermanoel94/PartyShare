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
