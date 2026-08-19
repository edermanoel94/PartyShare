# Toolchain da libwebrtc (M3)

Este documento registra o resultado do spike do M3.
O objetivo do marco era descobrir, antes de qualquer investimento em features, se um libwebrtc pré-compilado é utilizável nas três plataformas.

## 1. Resultado

| Plataforma | Status | Observação |
| --- | --- | --- |
| Linux x64, pré-compilado | Validado com ressalva | Compila, linka e executa, mas só com a libc++ do Chromium. Detalhes na seção 4. |
| Linux x64, build do fonte | Validado | Build concluído com `use_custom_libcxx=false`, spike revalidado sobre ele, inclusive captura de um frame real. Detalhes na seção 5. |
| Windows x64 | Não validado | Sem máquina Windows disponível no ambiente do spike. |
| macOS ARM64 | Não validado | Sem máquina macOS disponível no ambiente do spike. |
| macOS x64 | Bloqueado | A distribuição não publica build para essa arquitetura. |

O spike está em `tools/webrtc_spike/` e é habilitado com `-DDV_ENABLE_WEBRTC_SPIKE=ON`.
Ele verifica threads, `PeerConnectionFactory`, geração de SDP offer, enumeração de monitores, captura de um frame de tela, enumeração de dispositivos de áudio e a travessia de `std::string` pela fronteira da biblioteca.

Saída real em Linux, sem servidor gráfico anexado:

```text
libwebrtc toolchain spike

[ OK ] threads started
[ OK ] peer connection factory
[ OK ] peer connection
[ OK ] sdp offer                    5790 bytes
[ OK ] screen capturer              monitors found: 1
        monitor id=382 title="DP-2"
[ OK ] screen capture frame         1920x1080, 8100 KiB
[ OK ] audio device module          inputs: 2, outputs: 2
[ OK ] std::string across ABI       dv::shared linked and interoperating

spike passed
```

Essa execução é sobre a árvore construída do fonte, em uma sessão X11.
Sem servidor gráfico anexado as duas linhas de captura viram `skipped`, e o resto continua igual.

O SDP gerado traz `opus/48000/2` com `transport-cc`, além de H.264, VP8, VP9 e AV1, o que confirma que os codecs exigidos pela SPEC estão presentes no build.

## 2. Versão fixada

```text
release:  m152.7977.0.0
origem:   https://github.com/shiguredo-webrtc-build/webrtc-build
```

Os checksums SHA-256 estão em `cmake/Findlibwebrtc.cmake`.
Eles vêm dos digests publicados pela API do GitHub, então nada precisa ser baixado para conferir uma versão nova.
Para atualizar:

```sh
scripts/webrtc_checksums.sh                 # última release
scripts/webrtc_checksums.sh m153.0000.0.0   # uma release específica
```

## 3. Como reproduzir

```sh
cmake -S . -B build/spike \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DDV_ENABLE_WEBRTC_SPIKE=ON \
  -DDV_BUILD_CLIENT=OFF -DDV_BUILD_TESTS=OFF
cmake --build build/spike
./build/spike/bin/webrtc-spike
```

O download é de aproximadamente 110 MB no Linux, 322 MB no macOS e 739 MB no Windows.
Para reaproveitar uma árvore já extraída, use `-DDV_WEBRTC_ROOT=/caminho/para/webrtc`.

## 4. Armadilhas do pacote pré-compilado

Todas foram descobertas rodando o spike, e todas estão codificadas em `cmake/Findlibwebrtc.cmake`.
As duas primeiras valem só para os binários publicados.
As duas últimas valem também para a árvore construída do fonte.

### 4.1 O arquivo extrai para um subdiretório

O tarball extrai para `webrtc/`, contendo `include/`, `lib/libwebrtc.a`, `VERSIONS` e `DEPS`.
O módulo procura os headers e a biblioteca nos dois layouts, para também aceitar uma árvore construída à mão.

### 4.2 A libc++ do Chromium, com os headers incompletos

O build usa a libc++ do Chromium, cujos símbolos ficam no namespace de ABI `std::__Cr`, e não `std::__1` nem libstdc++.
Verificação:

```sh
nm -C --defined-only lib/libwebrtc.a | grep -oE 'std::__[A-Za-z0-9]+::' | sort -u
# std::__Cr::
```

O arquivo traz essa libc++ em `include/third_party/libc++/src/include`, mas com todos os headers sem extensão removidos.
Sobram apenas os diretórios internos `__*` e os headers de compatibilidade com C.
Ou seja, `<string>`, `<vector>` e `<cstdio>` não existem ali, e a árvore é inutilizável sozinha.

A solução é buscar o conjunto completo de headers no commit exato que o build fixa, que está em `VERSIONS`:

```text
WEBRTC_SRC_THIRD_PARTY_LIBCXX_SRC_COMMIT=5abc7f839700f0f17338434e1c1c6a8c87c00c11
```

O módulo lê esse commit do próprio arquivo e baixa `include.tar.gz` do espelho do Chromium, cerca de 1,8 MB.
Faltam ainda dois headers que normalmente são gerados pelo build da libc++, e que o módulo escreve:

- `__config_site`, com `_LIBCPP_ABI_NAMESPACE __Cr` e `_LIBCPP_ABI_VERSION 2`.
- `__assertion_handler`, com um handler vazio.

Consumidores compilam com `-nostdinc++` mais o include dessa árvore, e linkam com `-nostdlib++`, porque os símbolos de runtime da libc++ já estão dentro de `libwebrtc.a`.

### 4.3 O abseil do pacote precisa vir antes do abseil do sistema

Os headers públicos da libwebrtc incluem `absl/...`.
Se o abseil do sistema for encontrado primeiro, a compilação falha dentro de `absl/strings/internal/str_format` com erros sobre `std::basic_ostream` e `std::wstring`, porque as duas cópias discordam sobre a configuração da biblioteca padrão.
O módulo coloca `include/third_party/abseil-cpp` antes de `include`.

### 4.4 WEBRTC_USE_X11 e WEBRTC_USE_PIPEWIRE mudam o layout de structs públicas

Esta é a mais perigosa de todas, porque falha em silêncio.

`DesktopCaptureOptions` tem membros dentro de `#if defined(WEBRTC_USE_X11)` e `#if defined(WEBRTC_USE_PIPEWIRE)`.
A biblioteca foi construída com os dois definidos.
Um consumidor que não os define compila sem nenhum aviso, e depois corrompe a própria pilha quando a biblioteca escreve no objeto:

```text
*** stack smashing detected ***: terminated
#5  __stack_chk_fail ()
#6  check_screen_capture ()
```

O módulo define os dois como requisitos de uso da INTERFACE, então qualquer alvo que linke `libwebrtc::libwebrtc` os recebe automaticamente.

### 4.5 O caminho do portal exige glib, gbm e libdrm no link

Consequência direta de `WEBRTC_USE_PIPEWIRE`, e portanto do suporte a Wayland que o M6 precisa.

O código de `modules/desktop_capture` que fala com o portal do XDG usa GDBus, e importa os frames capturados como DMA-BUF.
Isso deixa referências a `g_dbus_*`, `g_variant_*`, `gbm_*` e `drm*` dentro de `libwebrtc.a`, que o consumidor precisa resolver:

```text
undefined reference to `g_dbus_proxy_new_finish'
undefined reference to `gbm_create_device'
undefined reference to `drmGetDevices2'
```

O módulo resolve isso com `pkg-config`, pedindo `glib-2.0`, `gio-2.0`, `gobject-2.0`, `gbm` e `libdrm`.
O PipeWire em si não entra na lista, porque a libwebrtc o carrega com `dlopen` em tempo de execução.

### 4.6 O alvo `webrtc` do GN não é a biblioteca inteira

Esta vale só para a árvore construída do fonte, e é a razão de o empacotamento do `build_webrtc.sh` não ser um `cp`.

`ninja webrtc` produz `obj/libwebrtc.a`, que parece completo mas não é.
`CreateBuiltinVideoEncoderFactory` e `CreateBuiltinVideoDecoderFactory` moram em alvos próprios do GN, e sem eles o cliente não linka:

```text
undefined reference to `webrtc::CreateBuiltinVideoEncoderFactory()'
undefined reference to `webrtc::CreateBuiltinVideoDecoderFactory()'
```

O script constrói esses alvos junto, expande cada um para o fecho transitivo das suas dependências com `gn desc ... deps --all`, e acrescenta ao arquivo os objetos que ainda não estão lá.
No estado atual isso são 88 objetos vindos de 448 arquivos, e a lista se ajusta sozinha quando um alvo novo entrar em `EXTRA_TARGETS`.

A comparação é feita por nome de objeto, porque um arquivo `.a` gordo só guarda o nome.
Se dois objetos diferentes tivessem o mesmo nome, um seria descartado, e isso apareceria como referência indefinida no link, nunca como um binário silenciosamente errado.

## 5. Consequência arquitetural: libc++ contra libstdc++ no Linux

Esta é a descoberta mais importante do M3, e afeta o M4 em diante.

No Linux, os binários publicados exigem que qualquer código que troque tipos `std::` com a libwebrtc seja compilado com a libc++ do Chromium.
A API pública da libwebrtc usa `std::string`, `std::vector` e `std::unique_ptr` por toda parte, então essa troca acontece o tempo todo, e não é evitável.

O problema é que o Qt 6 das distribuições Linux é construído contra libstdc++.
Um único binário não pode usar as duas bibliotecas padrão para os mesmos tipos.

Foram consideradas três saídas:

1. **Construir a libwebrtc a partir do fonte com `use_custom_libcxx=false`.**
   Passa a usar a biblioteca padrão do sistema e o conflito desaparece.
   Custo: um checkout acima de 30 GB, um build de dezenas de minutos, e a responsabilidade de manter e distribuir esse binário.

2. **Isolar a libwebrtc atrás de uma ABI em C, dentro de uma biblioteca compartilhada separada.**
   O processo passa a ter as duas bibliotecas padrão, mas nenhum tipo `std::` cruza a fronteira.
   Custo: uma camada de tradução para toda a superfície de mídia, mantida à mão para sempre.

3. **Compilar todo o cliente, inclusive o Qt, contra a libc++ do Chromium.**
   Inviável na prática: exigiria recompilar o Qt e todas as dependências.

**Decisão: opção 1.**
`scripts/build_webrtc.sh` automatiza o processo e empacota o resultado no layout que o `Findlibwebrtc.cmake` já consome.
Os argumentos de GN que importam:

```text
use_custom_libcxx=false     # a razão de existir do script
use_rtti=true               # nosso código e o Qt usam RTTI
rtc_use_h264=true           # H.264 é exigido pela seção 6 da SPEC
proprietary_codecs=true
ffmpeg_branding="Chrome"
use_sysroot=false           # linka contra a glibc desta máquina
```

O script escreve um arquivo marcador `DV_SYSTEM_LIBCXX` na árvore de saída.
O `Findlibwebrtc.cmake` procura por ele e desliga automaticamente todo o tratamento de libc++ descrito na seção 4.2.

### Resultado do build do fonte

O build foi concluído no Linux x64 e o resultado confirma a decisão.

A biblioteca não tem mais um único símbolo no namespace `std::__Cr`:

```sh
nm -C --defined-only lib/libwebrtc.a | grep -oE 'std::__[A-Za-z0-9]+::' | sort -u
# std::__cxx11::
# std::__detail::
```

`std::__cxx11` é a ABI do libstdc++, a mesma que o Qt 6 das distribuições usa.
O arquivo final tem cerca de 66 MB.

Dois detalhes do build merecem registro, porque nenhum dos dois é evidente:

- A biblioteca padrão usada na compilação é fixada em uma libstdc++ 14 baixada à parte, e não a do sistema.
  A faixa utilizável é estreita: mais antiga não tem os recursos de C++20 que a WebRTC usa, e mais nova faz o clang e o libstdc++ discordarem sobre `std::is_constructible`.
  Nesta máquina, com GCC 16, a compilação quebra em `call/rtp_config.cc` com um `std::optional::emplace` que o clang considera não construtível.
- As relocações CREL do Chromium são desligadas com `dv_disable_crel=true`.
  Só o lld as lê, e mantê-las obrigaria todo consumidor, inclusive o cliente Qt, a linkar com lld.

Ambos são aplicados pelo patch em `patches/webrtc/build/`, que o script aplica sozinho.

### 5.1 Patches que o projeto carrega

Ficam em `patches/webrtc/<repo>/`, onde `<repo>` é o checkout do gclient a que se aplicam.
O `build_webrtc.sh` aplica todos sozinho e falha cedo se algum não aplicar, o que é o sinal de que o milestone fixado se moveu.

| Patch | Por quê |
| --- | --- |
| `build/0001-libstdcxx-and-crel-opt-outs.patch` | Permite apontar para uma libstdc++ externa e desligar as relocações CREL, descrito acima. |
| `src/0001-qualify-nullptr-t.patch` | Correção de compilação com a libstdc++ fixada. |
| `src/0002-pulse-adm-reset-quit-on-init.patch` | Bug no dispositivo PulseAudio, descrito abaixo. |

### 5.2 O bug de captura do PulseAudio

Vale registrar porque custou tempo e porque é um bug real da libwebrtc, não do projeto.

Sintoma: a primeira chamada de um processo funciona, e toda chamada seguinte demora exatos dez segundos para negociar e fica sem microfone.

```text
(audio_device_pulse_linux.cc:1084): failed to activate recording
(thread.cc:551): Message to dv-worker took 10001ms to dispatch.
```

Causa: `AudioDeviceLinuxPulse::Terminate()` marca `quit_ = true`, e o `Init()` nunca volta essa flag para `false`.
Na segunda inicialização, a thread de captura recém-criada lê `quit_` na primeira passagem e termina imediatamente.
O `StartRecording()` então espera dez segundos por um evento que aquela thread deveria sinalizar, desiste, e a sessão fica sem captura.

O patch põe `quit_ = false` no `Init()`.
Com ele, sessões sucessivas no mesmo processo levam 1,2 s em vez de 10,2 s, e todas capturam áudio.
Vale reportar para o upstream.

### Como o spike verifica isso

O spike linka `dv::shared` quando a árvore da libwebrtc foi construída contra a biblioteca padrão do sistema, e serializa e reanalisa uma mensagem do protocolo passando pela fronteira.
Se as duas bibliotecas padrão fossem incompatíveis, isso falharia no link ou corromperia a string.
Sobre a árvore do fonte a linha diz `dv::shared linked and interoperating`, que é a confirmação de que o conflito acabou.

Em uma árvore pré-compilada no Linux, o spike é construído standalone e reporta a limitação em vez de falhar.
A linha `std::string across ABI` da saída diz qual dos dois casos ocorreu.

### Windows e macOS

Provavelmente têm o mesmo problema, porque o Chromium usa a própria libc++ nas três plataformas por padrão.
Isso ainda não foi confirmado, e é uma das coisas que [webrtc-validation.md](webrtc-validation.md) manda verificar.

## 6. O que falta para fechar o M3

- [x] Fixar versão e checksums verificáveis.
- [x] `Findlibwebrtc.cmake` com download, verificação e alvo importado.
- [x] Spike compilando, linkando e executando no Linux.
- [x] Decidir como resolver o conflito de biblioteca padrão: compilar do fonte, opção 1 da seção 5.
- [x] `scripts/build_webrtc.sh` automatizando esse build.
- [x] Spike capaz de detectar o conflito por si só, linkando `dv::shared`.
- [x] Rodar o build do fonte até o fim e revalidar o spike sobre ele.
- [x] Validar a captura de tela em X11, com servidor gráfico anexado.
      Um monitor enumerado e um frame de 1920x1080 capturado de verdade.
- [ ] Validar a captura de tela em Wayland, pelo portal do XDG.
      A máquina de desenvolvimento roda X11, então isso depende de outra sessão.
- [ ] Spike executando no Windows x64, ver [webrtc-validation.md](webrtc-validation.md).
- [ ] Spike executando no macOS ARM64.

O macOS x64 continua fora: a distribuição não publica esse build.
A SPEC o lista como desejável, não obrigatório, e ele volta ao radar no M9.
