# Avisos de entrada e saída da sala

Quando alguém entra ou sai de uma sala, o cliente avisa de duas formas:

| | Entrada | Saída |
| --- | --- | --- |
| **Notificação** — balão + piscar da barra de tarefas | se a janela não estiver em foco | não |
| **Som** — um chime | se o balão *não* subiu | sempre |

Sempre exatamente um som por evento. O balão traz o som do sistema junto e não
dá para pedir que ele venha calado (seção 1), então o chime cede a vez quando
ele sobe. Com a janela em foco não há balão, e o chime toca.

A assimetria é de propósito. Um chime é um aviso **da** sala, e quem mais
precisa dele é quem está olhando uma tela compartilhada com a lista de
participantes fora de vista. Uma notificação é um aviso **sobre** a sala, e é
inútil para quem já está olhando a lista. E alguém saindo é notícia para a
sala, não para o sistema operacional — o chime já disse, e um balão para cada
saída seria uma fila deles no fim de toda chamada.

O chime pode ser desligado: `[ui] room_sounds` no `config.ini`, ou a caixa
**Room sounds** na tela de Configurações. Ligado por padrão — um aviso que
ninguém pediu é mais fácil de desligar do que um aviso que ninguém sabe que
existe é de descobrir. O balão não tem interruptor nosso; quem quiser silenciá-lo
usa o do próprio sistema.

## 1. Notificação: `QSystemTrayIcon`, e o que isso custa

`ui::Notifier` usa duas coisas do Qt e mais nada:

- `QSystemTrayIcon::showMessage`, o balão na área de notificação;
- `QApplication::alert`, que acende o botão na barra de tarefas.

Ambas são portáteis e não custam nada para empacotar. **Isto é uma decisão, não
um padrão que ninguém questionou** — o caminho nativo do Windows foi escrito,
funcionou, e foi removido. O que se ganhou e o que se perdeu:

**Ganhou-se:** nenhum atalho é criado no Menu Iniciar do usuário. O caminho
nativo exigia um — a
[documentação da Microsoft](https://learn.microsoft.com/windows/win32/shell/enable-desktop-toast-with-appusermodelid)
é categórica de que um aplicativo desktop desempacotado só levanta toast se
existir um atalho carregando `System.AppUserModel.ID`, e a plataforma descarta
em silêncio o toast de um processo que ela não sabe nomear. Criar esse atalho
era uma alteração real na máquina de quem instala. Também sumiram cinco libs de
COM/WinRT do link e um arquivo de ~290 linhas.

**Perdeu-se, e vale saber:**

1. **A entrega não é garantida.** A documentação do `QSystemTrayIcon` diz, com
   todas as letras: *"messages may not appear at all. Hence, it should not be
   relied upon as the sole means for providing critical information."* Por isso
   o `QApplication::alert` é levantado junto e sempre — é a metade em que dá
   para confiar.
2. **O balão toca o próprio som, e não há como pedir silêncio.** No Windows
   isso é o balão legado do `Shell_NotifyIcon`, que o shell converte em toast e
   sonoriza. A API nativa aceitava `<audio silent="true"/>`; `showMessage` não
   tem parâmetro para isso.

   Como não dá para calar o balão, cala-se o chime: `Notifier::notify` devolve
   se um balão subiu, e `MainWindow::apply_participants` só toca o chime quando
   não subiu. Custo disso: o som de uma entrada **depende do foco** — o chime
   com a janela na frente, o do sistema com ela atrás. Dois sons diferentes
   para o mesmo evento, que é menos ruim que dois sons ao mesmo tempo. Note
   que o `false` não promete que o balão foi *visto* — a doc do Qt avisa que
   ele pode não aparecer —, e é por isso que o `alert` sobe de qualquer jeito.
3. **O ícone na bandeja passou a ser permanente.** Antes ele era criado só
   quando o caminho nativo falhava; agora ele *é* o canal, e um balão precisa
   de um ícone para pendurar. É criado no construtor do `Notifier` — adicionar
   o ícone e pedir que ele fale no mesmo instante é o tipo de ordem que
   funciona só na máquina onde foi escrita.

O ícone permanente é por que `main()` agora instala um `QApplication::windowIcon`
a partir dos recursos. No Windows o `.rc` já dá uma imagem para o Explorer e
para a barra de tarefas, mas aquela não é alcançável como `QIcon`.

## 2. Som: `PlaySound`, e por que não `QSoundEffect`

A resposta do Qt seria `QSoundEffect`, que vive no **Qt Multimedia**. No
Windows o Qt Multimedia toca pelo backend FFmpeg, então dois chimes de um
quinto de segundo colocariam um módulo e seus plugins de codec no build e no
instalador. O Qt instalado na máquina de build sequer traz o módulo.

`PlaySound` do `winmm` faz o mesmo em cinco linhas e é o que
`client/src/ui/sound_windows.cpp` usa. Três detalhes:

- **`SND_MEMORY`** — o som é compilado no executável, não instalado ao lado.
  Escrever o recurso num arquivo temporário só para poder nomeá-lo num caminho
  seria um arquivo temporário para limpar.
- **`SND_NODEFAULT`** — sem isso, um buffer que o `PlaySound` não consegue
  interpretar é respondido com o beep padrão da máquina, o que transformaria um
  asset quebrado num som que parece funcionar. Testado: um buffer de lixo volta
  `false`.
- **`SND_ASYNC`, e o buffer tem que sobreviver ao som.** O `PlaySound` lê do
  buffer na thread dele. Por isso `chimes.cpp` guarda os bytes num `static`
  local de função: é o menor tempo de vida obviamente longo o bastante.

Uma voz só, e isso é de propósito: uma segunda chamada corta a primeira, então
uma rajada de entradas é um som e não uma pilha deles.

Em Linux e macOS `ui::play_wav` devolve `false` (`sound_stub.cpp`) e o
`chimes.cpp` cai em `QApplication::beep()` — um som para os dois eventos, o que
não distingue entrar de sair. É o estado honesto até alguém escrever as metades
de ALSA e CoreAudio.

## 3. Os sons

`assets/sounds/joined.wav` e `left.wav`, gerados por
`assets/sounds/make_chimes.py` — mesmo padrão de `assets/ui/make_arrows.py`:
script versionado, saída versionada, fora do build.

São o mesmo intervalo — uma quinta justa, E5 e B5 — tocado nas duas direções.
Subindo é alguém chegando, descendo é alguém saindo; a convenção é velha o
suficiente para ninguém precisar aprender. 200 ms, PCM 16 bits mono, ~17 KB
cada, com envelope de cosseno elevado nas duas pontas de cada tom: uma senoide
que começa e termina em amplitude cheia estala, e o estalo é a parte que as
pessoas ouvem.

O pico é baixo (0.22). Isso toca por cima de uma chamada que alguém está
ouvindo, e um aviso que precisa competir com uma voz é um aviso que a
interrompe.

## 4. Quando o aviso sai

O servidor não manda evento de entrada nem de saída: ele transmite a lista
inteira de participantes toda vez que ela muda. Entrada e saída são, portanto,
as duas metades da diferença entre duas listas, e é assim que
`MainWindow::apply_participants` as calcula — a entrada por quem está na lista
nova e não estava na velha, a saída pelo contrário, que é a metade que a lista
nova não consegue mostrar porque quem saiu não está nela.

Quatro regras:

- **A primeira lista depois de entrar numa sala não avisa nada.** Ela é quem já
  estava lá, e anunciá-la receberia quem entra numa sala de cinco pessoas com
  cinco balões e cinco chimes. Ela semeia o conjunto e cala.
- **A própria pessoa nunca é anunciada.** A lista inclui quem a está lendo.
- **Um som por atualização, não um por pessoa.** E uma atualização que traz
  entrada e saída ao mesmo tempo toca a da entrada: a plataforma tem uma voz
  só, e dois avisos falando por cima um do outro dizem menos que um.
- **Nada de balão com a janela em foco.** Quem está olhando a lista viu a linha
  aparecer.
- **A notificação é decidida antes do som**, porque é a resposta dela que diz
  se o chime deve tocar.
