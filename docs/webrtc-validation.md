# Validação da libwebrtc no Windows e no macOS

Este documento é o roteiro para você fechar os itens do M3 que o ambiente de desenvolvimento não alcança.
O que falta validar está listado na seção 6 de [webrtc-toolchain.md](webrtc-toolchain.md).

## 1. O que precisa ser respondido

| Pergunta | Onde |
| --- | --- |
| A libwebrtc linka e roda no Windows x64? | Máquina Windows |
| A libwebrtc linka e roda no macOS ARM64? | Máquina macOS |
| A captura funciona em Wayland, e não só em X11? | Linux com sessão Wayland |
| `std::string` atravessa a fronteira da libwebrtc intacta? | Windows e macOS |

No Linux as duas últimas já têm resposta em X11, sobre a árvore construída do fonte.
A captura enumerou o monitor e entregou um frame de 1920x1080, e a `std::string` atravessou a fronteira intacta.
O que falta no Linux é a mesma verificação em uma sessão Wayland.

A pergunta sobre a `std::string` é a mais importante das quatro.
Ela é o conflito descrito na seção 5 de [webrtc-toolchain.md](webrtc-toolchain.md), confirmado no Linux com o pacote pré-compilado e resolvido lá pelo build do fonte.
O spike testa isso diretamente: quando o `dv::shared` é linkado junto, ele serializa e reanalisa uma mensagem do protocolo passando pela fronteira.

## 2. Importante sobre a sessão

Rode em uma sessão gráfica normal, na frente da máquina.

Em console de texto ou em runner de CI não vale: sem servidor gráfico anexado, `CreateScreenCapturer` devolve nulo e o spike reporta a captura como pulada, que é exatamente o que já sabemos e não acrescenta nada.

Por SSH vale pela metade.
Se houver uma sessão X11 rodando na máquina, apontar o spike para ela funciona e a captura é real:

```sh
DISPLAY=:1 XAUTHORITY=$HOME/.Xauthority ./build/spike/bin/webrtc-spike
```

Foi assim que a validação de X11 no Linux foi feita.
Em Wayland isso não se aplica: o portal do XDG precisa exibir um diálogo de consentimento na sessão do usuário, então ali é preciso estar na frente da máquina mesmo.

## 3. Linux, em sessão gráfica

```sh
scripts/validate_webrtc.sh
```

Depois repita em uma sessão do outro tipo.
Se você usa Wayland, entre uma vez em X11, e vice-versa, porque os dois caminhos de captura são implementações diferentes:

```sh
echo $XDG_SESSION_TYPE   # deve dizer wayland em uma das rodadas e x11 na outra
```

Em Wayland a captura passa pelo portal do XDG e o sistema deve exibir um diálogo de permissão.
Se o diálogo não aparecer e a captura falhar, isso é informação relevante, anote.

## 4. macOS ARM64

```sh
scripts/validate_webrtc.sh
```

Na primeira execução o macOS deve pedir permissão de gravação de tela.
Conceda em Ajustes do Sistema, Privacidade e Segurança, Gravação de Tela, e rode de novo.
Sem essa permissão a enumeração de monitores devolve uma lista vazia, o que é diferente de falhar.

O download do pacote pré-compilado é de 322 MB.

## 5. Windows x64

Não há script de shell aqui, porque a máquina não tem bash por padrão.
Abra o **x64 Native Tools Command Prompt for VS 2022** e rode:

```bat
cd C:\caminho\para\tudo-puta

cmake -S . -B build\spike -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDV_ENABLE_WEBRTC_SPIKE=ON ^
  -DDV_BUILD_CLIENT=OFF ^
  -DDV_BUILD_SERVER=OFF ^
  -DDV_BUILD_TESTS=OFF

cmake --build build\spike

build\spike\bin\webrtc-spike.exe
```

O download do pacote pré-compilado é de 739 MB, então a primeira configuração demora.

Se o Ninja não estiver instalado, troque `-G Ninja` por `-G "Visual Studio 17 2022" -A x64`, e o binário sai em `build\spike\bin\Release\`.

## 6. Como é uma execução boa

```text
libwebrtc toolchain spike

[ OK ] threads started
[ OK ] peer connection factory
[ OK ] peer connection
[ OK ] sdp offer                    5790 bytes
[ OK ] screen capturer              monitors found: 2
        monitor id=0 title="DELL U2720Q"
        monitor id=1 title="Built-in Retina Display"
[ OK ] screen capture frame         2560x1440, 14400 KiB
[ OK ] audio device module          inputs: 3, outputs: 4
[ OK ] std::string across ABI       dv::shared linked and interoperating

spike passed
```

As três linhas que interessam mais:

- `screen capturer` precisa dizer `monitors found: N` com N maior que zero.
  `skipped` significa que não havia sessão gráfica e o teste não valeu.
- `screen capture frame` precisa trazer uma resolução.
  Essa é a linha que prova que a captura entrega pixels, e não apenas que ela consegue listar monitores.
  Em Wayland ela só aparece depois de você aceitar o diálogo do portal, e o spike espera até 15 segundos por isso.
- `std::string across ABI` precisa dizer `dv::shared linked and interoperating`.

Sobre essa última linha, o sintoma do conflito é diferente por plataforma:

- No **Linux com pacote pré-compilado** o spike é construído standalone de propósito, e a linha diz `skipped`.
  Isso é esperado e já é o conflito conhecido.
  A validação real no Linux é sobre a árvore que o `scripts/build_webrtc.sh` produz, e já foi feita: o spike passa sobre ela com `dv::shared` linkado.
- No **Windows e no macOS** o spike tenta linkar o `dv::shared` direto.
  Se o conflito existir ali também, ele **não compila**: o link falha com símbolos `std::__Cr::`.
  Essa falha é justamente o resultado que precisamos saber, então mande a saída em vez de tentar contornar.

## 7. Se falhar

Mande a saída completa, inclusive os erros de compilação ou de link.
Erro de link mencionando símbolos com `std::__Cr::` é a assinatura exata do conflito de biblioteca padrão.

Vale mandar também:

```sh
uname -a                    # Linux e macOS
cmake --version
```

## 8. Compilando do fonte nessas plataformas

Se o item 5 falhar no Windows ou no macOS, aquela plataforma precisa do mesmo tratamento do Linux.

No macOS o mesmo script serve:

```sh
scripts/build_webrtc.sh
scripts/validate_webrtc.sh --root ~/.cache/desktop-voice/webrtc/dist
```

No Windows o build exige Visual Studio com o Windows SDK e as ferramentas do depot_tools, e o procedimento é o descrito na documentação oficial da WebRTC.
Vale registrar aqui quando for feito pela primeira vez.

Em ambos os casos, reserve tempo: o checkout passa de 30 GB e o build leva dezenas de minutos.
