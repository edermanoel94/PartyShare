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

O caso está em `tests/integration/test_benchmark.cpp`.
`DV_BENCHMARK_SECONDS` controla quanto tempo a chamada é mantida; o padrão é 30 segundos.

## Cinco participantes, um compartilhando a tela

Uma sala de cinco, todos com o microfone aberto, um deles compartilhando o monitor em 1280x720 a 30 FPS.
Medido depois que todos os quatro espectadores já estavam recebendo quadros, para que o que se mede seja a chamada em regime.

### 30 segundos

```text
CPU, cinco clientes        82,9% de um núcleo
CPU, por cliente           16,6% de um núcleo
memória, cinco clientes    152 MiB
memória, por cliente        30 MiB

participante   rtt      jitter   perdidos   fps      resolução
ana (envia)      1 ms   2,0 ms          0      -     -
bruno            1 ms   2,0 ms          0   29,8     1280x720
carla            1 ms   2,0 ms          0   29,8     1280x720
diego            1 ms   2,0 ms          0   29,8     1280x720
elena            1 ms   2,0 ms          0   29,8     1280x720

SFU: 30.369 pacotes de áudio encaminhados, 3.924 de vídeo
```

### 10 minutos

O critério do M6 pede FPS estável por dez minutos. `DV_BENCHMARK_SECONDS=600`:

```text
CPU, cinco clientes        85,2% de um núcleo
CPU, por cliente           17,0% de um núcleo
memória, cinco clientes    174 MiB
memória, por cliente        35 MiB

participante   rtt      jitter   perdidos   fps      resolução
ana (envia)      1 ms   3,0 ms          0      -     -
bruno            1 ms   3,0 ms          0   30,0     1280x720
carla            1 ms   3,0 ms          0   30,0     1280x720
diego            1 ms   3,0 ms          0   30,0     1280x720
elena            1 ms   3,0 ms          0   30,0     1280x720

SFU: 600.394 pacotes de áudio encaminhados, 74.216 de vídeo
```

Dez minutos a 30,0 FPS, com zero pacotes perdidos, e CPU praticamente igual à dos trinta segundos.

Duas ressalvas honestas sobre este número:

- **30,0 FPS é a média da janela inteira.** Ela não prova que não houve uma queda de meio segundo no meio. Uma medição por segundo, com desvio padrão, é o que responderia isso, e ainda não existe.
- **A memória subiu de 152 para 174 MiB entre a corrida de 30 s e a de 600 s.** São 22 MiB em dez minutos, com cinco clientes no mesmo processo. Pode ser regime permanente de buffers, e pode ser vazamento lento. Não foi investigado, e fica registrado aqui para ser.

### O que estes números não dizem

Os cinco clientes rodam **no mesmo processo e na mesma máquina**, sobre loopback.
Isso muda três coisas, e é importante que estejam escritas antes que alguém cite os números:

- **O rtt de 1 ms não é uma medida de rede.**
  É o custo do laço local. A meta de menos de 150 ms da seção 22 continua sem medição em rede de verdade, e é o que a tarefa 2 do M8 endereça com `tc netem`.
- **O custo por cliente é otimista.**
  A camada de mídia tem um `Engine` único por processo: um `AudioDeviceModule`, um módulo de processamento de áudio e três threads da libwebrtc, compartilhados pelas cinco sessões.
  Cinco processos separados custariam mais do que 16,6% de núcleo e 30 MiB cada.
- **A captura de tela acontece uma vez.**
  Só um participante compartilha, que é o que a seção 5.2 permite, então o custo de captura e codificação aparece uma vez e não cinco.

O que os números dizem com segurança é o formato do sistema: cinco participantes, quatro recebendo 1280x720 a praticamente 30 FPS, sem perda, com o SFU encaminhando trinta mil pacotes de áudio em trinta segundos.

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
| FPS | 30 | 29,8 em 30 s, 30,0 em 10 min |
| Participantes | 5 | 5 |
| Áudio | 48 kHz Opus | 48 kHz Opus |
| Vídeo | H.264 | H.264, OpenH264 em software |
| Latência | < 150 ms | 1 ms em loopback, sem medição em rede real |
| CPU | baixo consumo | 16,6% de um núcleo por cliente, com a ressalva acima |
| Memória | < 500 MB por cliente | 30 a 35 MiB por cliente, com as ressalvas acima |
| Startup | < 3 s | 18 a 26 ms |
