# Requisitos de hardware

O que uma máquina precisa ter para rodar o cliente e o servidor do PartyShare.

Os números da coluna "medido" saem de [benchmarks.md](benchmarks.md), na máquina de referência descrita lá.
O que não foi medido está marcado como estimativa, e estimativa aqui quer dizer conta feita a partir do bitrate configurado, não um palpite.

## Estado das plataformas

Vale ler isto antes das tabelas.

| Plataforma | Status |
| --- | --- |
| Linux x64 | Construído e executado, é onde todos os números desta página foram medidos |
| Windows x64 | Código e presets existem, nunca construído nem executado |
| macOS ARM64 | Código e presets existem, nunca construído nem executado |

Os requisitos de Windows e macOS abaixo seguem do que a libwebrtc e o Qt 6 exigem nessas plataformas, e não de uma execução real.

## Cliente

### Mínimo

| | |
| --- | --- |
| CPU | x64 com 2 núcleos, ou Apple Silicon |
| Memória | 2 GB livres |
| GPU | Qualquer uma capaz de compor a janela, o encoder por hardware é opcional |
| Áudio | Um dispositivo de entrada e um de saída, ou `DV_AUDIO_NULL_DEVICE=1` para rodar sem placa de som |
| Rede | 4 Mbps de descida e 4 Mbps de subida |
| Disco | 200 MB para o binário e as bibliotecas |

Dois núcleos é o mínimo porque a captura, a codificação, a rede e a UI rodam em threads separadas, conforme a seção 16 da SPEC.
Um núcleo faz a chamada funcionar e faz a interface engasgar.

### Recomendado

| | |
| --- | --- |
| CPU | x64 com 4 núcleos ou mais |
| Memória | 4 GB livres |
| GPU | NVIDIA com NVENC, para tirar a codificação do processador |
| Rede | 10 Mbps de descida e 5 Mbps de subida |

### Consumo medido

Numa sala de cinco, com um compartilhando a tela em 1280x720 a 30 FPS, codificando em software:

| | Medido | Meta da seção 22 da SPEC |
| --- | --- | --- |
| CPU por cliente | 15,6% a 17,2% de um núcleo | baixo consumo |
| Memória por cliente | 35 a 38 MiB | menos de 500 MB |
| Inicialização | 18 a 26 ms | menos de 3 s |

A ressalva importante: esses cinco clientes rodam no mesmo processo de teste, sem interface Qt.
O processo real carrega a UI e o toolkit por cima disso, então trate os 35 MiB como o custo da camada de mídia e não como o tamanho do aplicativo.

### Banda por cliente

O vídeo é configurado em 1500 a 3000 kbps e o áudio em 48 kbps, valores da `dv::config` e da seção 6 da SPEC.

| Situação | Subida | Descida |
| --- | --- | --- |
| Só ouvindo e falando | ~48 kbps | ~192 kbps, os outros quatro |
| Compartilhando a tela | ~3 Mbps | ~192 kbps |
| Assistindo alguém compartilhar | ~48 kbps | ~3,2 Mbps |

O controle de congestionamento reduz o vídeo sozinho quando o enlace não aguenta, até o piso de 300 kbps.
Um enlace abaixo disso não deixa a chamada cair, deixa a imagem ruim.

## Encoder por hardware

Opcional em toda máquina: sem ele o compartilhamento de tela é codificado pelo processador, com o custo de CPU da tabela acima.

| | |
| --- | --- |
| Backend | NVENC |
| Placa | NVIDIA com NVENC, o que hoje quer dizer Kepler ou mais nova |
| Driver | Precisa expor a API NVENC 13.1 ou mais nova, que é a versão do cabeçalho em `third_party/nvcodec` |
| Bibliotecas | `libnvidia-encode.so.1` e `libcuda.so.1`, ambas vindas do driver |

Nada disso é linkado: as bibliotecas são abertas em tempo de execução, então o mesmo binário roda numa máquina sem placa NVIDIA.
Quando não há hardware, o motivo é dito uma vez no log, na criação da engine.

Intel e AMD não têm backend por enquanto, e nessas máquinas a codificação é sempre em software.

## Captura de tela

A captura vem da própria libwebrtc, e o que ela exige do sistema é o que o PartyShare exige.

| Sistema | Backend | Requisito |
| --- | --- | --- |
| Windows | Windows Graphics Capture, com DXGI Desktop Duplication de fallback | Windows 10 1903 ou mais novo |
| macOS | ScreenCaptureKit | macOS 13 ou mais novo, e permissão de gravação de tela |
| Linux X11 | XComposite e XDamage | Servidor X com as extensões carregadas |
| Linux Wayland | Portal do XDG por PipeWire | `xdg-desktop-portal` com backend instalado, e consentimento do usuário a cada sessão |

No Linux, a captura em X11 já foi validada com servidor gráfico anexado.
A validação em Wayland está pendente, conforme o M3 do [../PLAN.md](../PLAN.md).

## Servidor

O servidor faz signaling e roteia mídia como SFU, sem transcodificar.
Isso quer dizer que ele gasta banda e quase nada de processador: um pacote que chega é copiado para os destinos e sai.

### Mínimo, para uma sala de cinco

| | |
| --- | --- |
| CPU | 1 núcleo x64 |
| Memória | 512 MB |
| Rede | 20 Mbps de saída e 5 Mbps de entrada |
| Disco | 100 MB, mais o que os logs ocuparem |
| Sistema | Linux x64, sem servidor gráfico e sem placa de som |

### Banda por sala

Com cinco participantes e um compartilhando a tela, e vídeo no teto de 3000 kbps:

| Sentido | Conta | Total |
| --- | --- | --- |
| Entrada | 1 vídeo de 3 Mbps mais 5 áudios de 48 kbps | ~3,3 Mbps |
| Saída | 4 cópias do vídeo mais 20 cópias de áudio | ~13 Mbps |

A saída cresce com o número de espectadores, e é ela que dimensiona a máquina.
Uma regra que serve para planejar: some 3,3 Mbps por espectador de tela e 200 kbps por participante em áudio.

### Portas

| Porta | Protocolo | Uso |
| --- | --- | --- |
| 8080 | TCP | Signaling por WebSocket, configurável com `--port` ou `DV_SERVER_PORT` |
| efêmeras | UDP | ICE e mídia, escolhidas pelo sistema a cada conexão |

O servidor precisa alcançar os clientes por UDP.
Atrás de NAT ele usa os servidores STUN configurados, e um TURN opcional para os casos que o STUN não resolve.

## Máquina de compilação

Compilar é mais pesado do que rodar, por causa da libwebrtc.

| | |
| --- | --- |
| CPU | Quanto mais núcleos, melhor, o build é paralelo |
| Memória | 16 GB, o link da libwebrtc é a parte que consome |
| Disco | 30 GB para o checkout da libwebrtc, mais o build do projeto |
| Tempo | Dezenas de minutos para a libwebrtc, na primeira vez |

O cliente sem a camada de mídia, o servidor e os testes compilam sem nada disso.
As ferramentas e as dependências estão em [build.md](build.md).
