# Benchmarks

As metas da seção 22 da SPEC, medidas.

Cada número aqui saiu de uma execução real registrada abaixo, com a máquina e o método ao lado.
Nada foi estimado, e o que ainda não foi medido está marcado como tal em vez de preenchido com um palpite.

## Máquina de referência

| | |
| --- | --- |
| CPU | AMD Ryzen 7 7435HS, 16 threads |
| Memória | 15 GiB |
| Sistema | Arch Linux, kernel 7.1.3 |
| Qt | 6.11.1 |
| libwebrtc | m152.7977.0.0, construída do fonte contra a libstdc++ do sistema |
| Rede | loopback |

## Como reproduzir

```sh
cmake --build build/media --target dv_benchmarks
ctest --test-dir build/media -L benchmark --output-on-failure
```

Os casos estão em `tests/integration/test_benchmark.cpp`.
`DV_BENCHMARK_SECONDS` controla quanto tempo a chamada é mantida; o padrão é 30 segundos.

As medições em rede degradada estão em `tests/integration/test_network_impairment.cpp` e rodam junto com a suíte de mídia:

```sh
ctest --test-dir build/media -L media --output-on-failure
```

## Cinco participantes, um compartilhando a tela

Uma sala de cinco, todos com o microfone aberto, um deles compartilhando o monitor em 1280x720 a 30 FPS.
Medido depois que todos os quatro espectadores já estavam recebendo quadros, para que o que se mede seja a chamada em regime.

### 30 segundos

```text
CPU, cinco clientes        86,0% de um núcleo
CPU, por cliente           17,2% de um núcleo
memória, cinco clientes    175 MiB
memória, por cliente        35 MiB

participante   rtt      jitter   perdidos   fps      resolução
ana (envia)      1 ms   3,0 ms          0      -     -
bruno            1 ms   3,0 ms          0   29,7     1280x720
carla            1 ms   3,0 ms          0   29,7     1280x720
diego            1 ms   2,0 ms          0   29,7     1280x720
elena            1 ms   3,0 ms          0   29,7     1280x720

SFU: 30.361 pacotes de áudio encaminhados, 3.912 de vídeo
```

### 10 minutos

O critério do M6 pede FPS estável por dez minutos. `DV_BENCHMARK_SECONDS=600`:

```text
CPU, cinco clientes        78,2% de um núcleo
CPU, por cliente           15,6% de um núcleo
memória, cinco clientes    190 MiB
memória, por cliente        38 MiB

participante   rtt      jitter   perdidos   fps      resolução
ana (envia)      1 ms   3,0 ms          0      -     -
bruno            1 ms   3,0 ms          0   30,0     1280x720
carla            1 ms   3,0 ms          0   30,0     1280x720
diego            1 ms   3,0 ms          0   30,0     1280x720
elena            1 ms   3,0 ms          0   30,0     1280x720

SFU: 600.332 pacotes de áudio encaminhados, 74.204 de vídeo
```

Dez minutos a 30,0 FPS, com zero pacotes perdidos, e CPU um pouco abaixo da corrida de trinta segundos - o custo de subir a chamada pesa mais numa janela curta.

Duas ressalvas honestas sobre este número:

- **30,0 FPS é a média da janela inteira.** Ela não prova que não houve uma queda de meio segundo no meio. Uma medição por segundo, com desvio padrão, é o que responderia isso, e ainda não existe.
- **A memória subiu de 175 para 190 MiB entre a corrida de 30 s e a de 600 s.** São 15 MiB em dez minutos, com cinco clientes no mesmo processo. Pode ser regime permanente de buffers, e pode ser vazamento lento. Não foi investigado, e fica registrado aqui para ser.

### O que estes números não dizem

Os cinco clientes rodam **no mesmo processo e na mesma máquina**, sobre loopback.
Isso muda três coisas, e é importante que estejam escritas antes que alguém cite os números:

- **O rtt de 1 ms não é uma medida de rede.**
  É o custo do laço local.
  A meta de menos de 150 ms da seção 22 continua sem medição em rede de verdade: o que a seção seguinte mede é latência injetada, que responde "a chamada aguenta meio segundo de ida e volta" e não "a chamada em rede real fica abaixo de 150 ms".
- **O custo por cliente é otimista.**
  A camada de mídia tem um `Engine` único por processo: um `AudioDeviceModule`, um módulo de processamento de áudio e três threads da libwebrtc, compartilhados pelas cinco sessões.
  Cinco processos separados custariam mais do que 17,2% de núcleo e 35 MiB cada.
- **A captura de tela acontece uma vez.**
  Só um participante compartilha, que é o que a seção 5.2 permite, então o custo de captura e codificação aparece uma vez e não cinco.

O que os números dizem com segurança é o formato do sistema: cinco participantes, quatro recebendo 1280x720 a praticamente 30 FPS, sem perda, com o SFU encaminhando trinta mil pacotes de áudio em trinta segundos.

## Rede degradada

A seção 22 pede que a chamada sobreviva a 5% de perda de pacotes com degradação suave e sem queda.
Isso é medido de dois jeitos, porque nenhum dos dois sozinho é honesto.

O primeiro é `scripts/netem.sh`, que aplica `tc netem` a uma interface.
É o mais fiel: degrada as filas do próprio sistema operacional, para todos os processos e nos dois sentidos.
Também precisa de root, só existe no Linux, e nesta máquina não roda: o kernel foi atualizado sem reiniciar, e os módulos do kernel em execução não existem mais, então `sch_netem` não carrega.
O script diagnostica exatamente esse caso em vez de deixar o `tc` responder "Specified qdisc kind is unknown".

O segundo é o injetor descrito em `client/src/media/network_impairment.hpp`, que danifica os pacotes nos próprios sockets UDP do cliente, abaixo do DTLS e acima do sistema operacional.
Não precisa de privilégio nenhum, roda nas três plataformas, e degrada exatamente o enlace de um participante com o SFU.
Tudo abaixo dele é real: Opus real, SRTP real, jitter buffer real, RTCP real.
O que é simulado é só o fio.

### Cinco participantes com 5% de perda

A mesma sala do benchmark acima, com 5% de perda injetada em cada sentido depois que a chamada já estava em regime.

```text
CPU, cinco clientes        80,8% de um núcleo
memória, cinco clientes    230 MiB

participante   rtt      jitter   perdidos   fps      resolução
ana (envia)      1 ms   3,0 ms        542      -     -
bruno            1 ms   3,0 ms        545   29,7     1280x720
carla            1 ms   3,0 ms        568   29,7     1280x720
diego            1 ms   3,0 ms        542   29,7     1280x720
elena            1 ms   2,0 ms        552   29,7     1280x720

injetado: 435 de 9.254 pacotes descartados na saída, 1.548 de 33.605 na entrada
reparo:   37 pedidos, 37 pacotes de vídeo faltando, 37 recuperados
```

Os quatro espectadores continuam recebendo 1280x720 a 29,7 FPS, o mesmo número da corrida sem perda.
O áudio perde os pacotes que foram descartados e o Opus preenche as lacunas.
Ninguém cai.

Comparando com a corrida limpa: CPU praticamente igual, e memória de 175 para 230 MiB.
Os 55 MiB a mais são o custo de segurar pacotes para retransmitir e de jitter buffers maiores, e não foram investigados além disso.

### Áudio, isolado

Dois participantes, 15 segundos, 5% de perda em cada sentido:

```text
injetado     95 de 1.619 pacotes na saída, 58 de 1.520 na entrada
receptor     677 pacotes chegaram, 60 contados como perdidos
qualidade    rtt 1 ms, jitter 2,0 ms
```

A perda que o receptor contabiliza é da ordem da que foi injetada, e não um múltiplo dela.
Isso é o que "degradação suave" quer dizer aqui: o Opus carrega um quadro por pacote, então 5% de pacotes perdidos são 5% do áudio.

### Latência e jitter

250 ms em cada sentido, com 30 ms de jitter, também por 15 segundos:

```text
pacotes segurados   3.216
receptor            715 pacotes chegaram, rtt 492 ms, jitter 19,0 ms
```

Os 492 ms medidos pela própria libwebrtc contra os 500 ms injetados são a verificação cruzada de que o injetor faz o que diz.
A chamada continua de pé, com a conversa desconfortável que meio segundo de ida e volta produz.

### O congelamento do compartilhamento de tela, encontrado aqui

Antes de qualquer correção, a mesma tela compartilhada em um enlace com 5% de perda entregava **4 quadros em 15 segundos**.
Não era degradação, era congelamento.

O motivo é aritmético.
Um quadro intra de uma tela em 1280x720 tem mais de cem pacotes, e a 5% de perda a chance de os cem chegarem inteiros é de menos de 1%.
O único reparo que um espectador tinha era pedir um novo quadro intra, que chegava quebrado, e o pedido recomeçava.
O SFU carregou oito pedidos de keyframe em quinze segundos e nenhum deles produziu imagem.

A correção tem duas partes, ambas no SFU:

- `rtc::RtcpNackResponder` na track de saída, que responde ao espectador que perdeu um pacote reenviando aquele pacote.
- `dv::server::sfu::VideoFeedback` na track de entrada, que faz o mesmo pedido para cima: quando um pacote do compartilhador se perde a caminho do SFU, o SFU pede a retransmissão em vez de deixar o buraco seguir para todos os espectadores.
  A libdatachannel responde NACK mas nunca pede um, e essa metade não existia.

Depois disso, o mesmo caso entrega **443 quadros em 15 segundos**, com 22 pacotes faltando, 22 recuperados e **zero** pedidos de keyframe.

A ressalva: a tela medida é a de uma sessão de trabalho parada, onde os quadros delta cabem em um pacote cada.
Uma tela com vídeo em movimento tem quadros de vários pacotes, e um pacote perdido custa o quadro inteiro até a retransmissão chegar.
O reparo continua valendo, mas o número de quadros por segundo sob perda seria menor, e isso não foi medido.

### Bitrate sob congestionamento

O compartilhamento de tela não envia sempre a mesma coisa: o quanto ele pode enviar é decidido pelo SFU e obedecido pelo cliente.
Como isso funciona está em [../PLAN.md](../PLAN.md) e em `server/src/sfu/bandwidth_estimator.hpp`; o que se mede aqui é o laço fechado.

Uma tela compartilhada, com 20% de perda ligada e depois desligada:

```text
enlace limpo      2.568 kbps
com 20% de perda  1.613 kbps, e o SFU pedindo 1.613
depois            2.193 kbps
```

Os dois números do meio são as duas pontas do mesmo laço: o SFU decide, o REMB carrega, e o controlador de congestionamento da libwebrtc obedece.
Se eles discordassem, o laço estaria quebrado em algum ponto entre um e outro.

A queda leva segundos e a recuperação leva dezenas, o que é de propósito: recuar tem que ser mais rápido do que sondar, ou o congestionamento dura mais do que precisa.

Antes disso, o número não se mexia.
Sem `transport-cc` e sem REMB, a estimativa do remetente subia até o teto e ficava lá, mesmo com um quinto dos pacotes sendo descartados: a libwebrtc não tem como saber de uma perda que ninguém lhe conta.

### Uma coisa que não deu para ligar

Falta a outra direção do laço: o espectador dizer ao SFU quanto o enlace dele aguenta.
O código está lá e é exercitado por teste, mas o cliente deste projeto nunca envia esse relatório, porque para produzi-lo a libwebrtc precisa da extensão `abs-send-time` no cabeçalho RTP.

Negociar essa extensão faz a libwebrtc sondar a banda, e sondar é enviar pacotes com padding.
A libdatachannel tem um `assert` que derruba o processo inteiro ao montar o sender report de um pacote com padding, então a extensão fica de fora até que isso seja resolvido no upstream.

O SFU passou a descartar pacotes com padding em vez de encaminhá-los, porque um cliente que este projeto não escreveu pode enviá-los de qualquer forma, e o resultado disso era o servidor abortando.

## Startup do cliente

Medido do início de `main` até a janela estar exibida, que é o mais perto que dá de chegar do que a pessoa espera.
O cliente registra isso em toda execução:

```text
Voice Desktop client ready in 18 ms
Voice Desktop client ready in 25 ms
Voice Desktop client ready in 26 ms
```

Meta da seção 22: menos de 3 segundos. Folgado.

## Estado das metas da seção 22

| Métrica | Meta | Medido |
| --- | --- | --- |
| Screen share | 1280x720 | 1280x720 |
| FPS | 30 | 29,7 em 30 s, 30,0 em 10 min |
| Participantes | 5 | 5 |
| Áudio | 48 kHz Opus | 48 kHz Opus |
| Vídeo | H.264 | H.264, OpenH264 em software |
| Latência | < 150 ms | 1 ms em loopback; sobrevive a 492 ms injetados; sem medição em rede real |
| Perda de pacotes | sobreviver a 5% | 29,7 FPS e chamada de pé com 5% injetados nos dois sentidos |
| CPU | baixo consumo | 15,6% a 17,2% de um núcleo por cliente, com a ressalva acima |
| Memória | < 500 MB por cliente | 35 a 38 MiB por cliente, com as ressalvas acima |
| Startup | < 3 s | 18 a 26 ms |
