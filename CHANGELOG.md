# Changelog

Every version PartyShare has published, newest first. A version is one merge to
master: `.github/workflows/tag.yml` writes the entry from the pull request title
when it bumps the version, so this list and the GitHub releases say the same
thing. The Windows installer shows it on its "What's new" page.

A version raised by hand in `CMakeLists.txt` skips that step, so whoever raises
it writes the entry here in the same commit.

## 0.1.59 (2026-09-07)

- O DMG do macOS sobrevive a um hdiutil ocupado

## 0.1.58 (2026-09-06)

- A tela de login pede o endereço do servidor e testa a conexão

## 0.1.57 (2026-09-06)

- O Settings volta a tela de login, sem sair da tela de escolher a sala

## 0.1.56 (2026-09-06)

- O instalador do Windows ganha a licenca MIT e uma pagina de novidades

## 0.1.55 (2026-09-06)

- Move o Settings da tela de login para a tela de escolher a sala

## 0.1.54 (2026-09-04)

- Sinaliza a sessao encerrada em vez de deixar o cliente na sala com "senha errada"

## 0.1.53 (2026-09-03)

- O Admin entra na sala e a barra de status para de cortar no meio da palavra

## 0.1.52 (2026-09-03)

- Etapas 6 a 12 do plano de áudio, numa branch só

## 0.1.51 (2026-09-03)

- O nível da supressão de ruído vira uma escolha

## 0.1.50 (2026-09-03)

- O controle de ganho passa a ser o de segunda geração

## 0.1.49 (2026-09-03)

- O áudio perdido passa a ser pedido de novo, nos dois trechos

## 0.1.48 (2026-09-03)

- O áudio passa a viajar com redundância

## 0.1.47 (2026-09-03)

- O .dmg do macOS passa a sair com áudio e vídeo

## 0.1.46 (2026-09-03)

- A tela para de congelar quando quem compartilha sai da sala

## 0.1.45 (2026-09-03)

- O dbadmin passa a avisar e a encerrar a sessão de quem está conectado

## 0.1.44 (2026-09-02)

- Sair da sala zera o mudo, e a tela compartilha um monitor escolhido

## 0.1.43 (2026-09-02)

- Avisa no rodape quando uma versao nova foi publicada

## 0.1.42 (2026-09-02)

- Instalador do servidor para Linux e o tarball do servidor na release

## 0.1.41 (2026-09-02)

- Capacidade por sala, ícones de estado e servidor offline na entrada

## 0.1.40 (2026-09-02)

- O primeiro da sala passa a ter microfone no Windows

## 0.1.39 (2026-09-02)

- Mensagem direta do administrador com OK, e presença com IP no dbadmin

## 0.1.38 (2026-09-02)

- O que a auditoria E2E no macOS encontrou: cinco consertos

## 0.1.37 (2026-08-28)

- O foco invisível, o crash ao sair e o nome que virava id

## 0.1.36 (2026-08-27)

- Os logs passam a dizer o nome de quem, e o nome da sala

## 0.1.35 (2026-08-27)

- O capitulo do release passa a dizer o que a tag realmente produz
- O teste do SFU desiste quando a conexao morre, em vez de esperar dez segundos

## 0.1.34 (2026-08-27)

- A documentacao vira um livro de quinze capitulos

## 0.1.33 (2026-08-27)

- O rodape passa a dizer qual build esta rodando

## 0.1.32 (2026-08-26)

- As salas ganham nome, e quem nao der um fica com o proprio codigo

## 0.1.31 (2026-08-26)

- A senha do usuario comum, trocada por ele mesmo

## 0.1.30 (2026-08-26)

- Uma restricao escrita fora do servidor passa a valer na sessao que ja esta aberta

## 0.1.29 (2026-08-26)

- Links clicaveis no chat, e um aviso quando alguem entra ou sai da sala

## 0.1.28 (2026-08-26)

- A tela que troca de dono e o microfone que volta mudo depois de uma reconexao

## 0.1.27 (2026-08-26)

- O som da tela ganha um volume, e a supressao de ruido ganha uma caixa

## 0.1.26 (2026-08-25)

- O dbadmin ganha uma tela de salas, e uma mudança no Go para de rodar o CI inteiro

## 0.1.25 (2026-08-25)

- Ver as salas não é administração, e o usuário comum tem direito a uma

## 0.1.24 (2026-08-25)

- Toda sala agora vai para o banco e sobrevive a quem estava nela

## 0.1.23 (2026-08-25)

- O estado da conexão do SFU só existia em debug, que release não compila

## 0.1.22 (2026-08-25)

- O IP do cliente nos logs de conexão do servidor

## 0.1.21 (2026-08-25)

- Trocar de servidor sem fechar o programa, e o status de rede de volta na tela principal

## 0.1.20 (2026-08-24)

- Nao imprimir a senha do banco quando a conexao falha

## 0.1.19 (2026-08-24)

- O gofmt acusava os dezessete arquivos do dbadmin, e nenhum estava torto

## 0.1.18 (2026-08-24)

- Ouvir o audio da tela compartilhada

## 0.1.17 (2026-08-24)

- O teto do bitrate nao acompanhava a resolucao, e 1080p60 saia pela metade

## 0.1.16 (2026-08-24)

- Validacao em Linux e macOS, e os cinco avisos que o clang-tidy achou

## 0.1.15 (2026-08-24)

- Salvar no settings, metricas em grafico, e a renderizacao mais fluida

## 0.1.14 (2026-08-24)

- Dois defeitos achados testando a release v0.1.13

## 0.1.13 (2026-08-24)

- Qualidade do compartilhamento de tela e codificacao por hardware no Windows

## 0.1.12 (2026-08-23)

- O slider de volume desabilitado pintava a laje inteira

## 0.1.11 (2026-08-23)

- O teste do dbadmin esperava a frase e olhava a tela atras dela

## 0.1.10 (2026-08-23)

- Uma interface com cantos, e um config.ini que nasce pronto e guarda o que voce escolhe

## 0.1.9 (2026-08-23)

- Moderação de sala: o que um administrador tira de uma conta, e não só de uma visita

## 0.1.8 (2026-08-22)

- Chat da sala, com a conversa vivendo exatamente enquanto a sala

## 0.1.7 (2026-08-22)

- O glob que so casava na maquina de quem escreveu

## 0.1.6 (2026-08-22)

- A chamada negociava tudo e nao carregava um pacote: duas copias da libsrtp num binario so

## 0.1.5 (2026-08-22)

- macOS: fazer a camada de midia linkar, e parar de publicar um bundle quebrado

## 0.1.4 (2026-08-21)

- A MSI do Windows nunca teve media layer dentro, e ninguém tinha por onde saber

## 0.1.3 (2026-08-21)

- A track existia e o teste mandava áudio nela; existir e estar aberta são momentos diferentes

## 0.1.2 (2026-08-21)

- Um atalho não carrega argumento, e era só assim que se dizia onde estava o servidor

## 0.1.1 (2026-08-21)

- As contas e a auditoria sem depender do servidor de pé
- A primeira build do Windows, e os dois defeitos que ela achou
- A assinatura cobria dois arquivos, e sete dos que faltavam são o motivo do bloqueio
- O MSI instala: correção do relato do #5
- O spike da libwebrtc roda no Windows, e o conflito de libc++ não existe lá
- A camada de mídia roda no Windows, sobre uma build do fonte
- Um caminho curto até um servidor de pé e dois clientes numa sala
- O cancelador de eco não estava desligado, e o patch não falhava como eu disse
- A tabela dizia qual flag desliga o cliente, mas não como subir só o servidor
- O SFU pedia uma porta efêmera por participante, e o firewall pagava a conta
- Subir a versão era algo que alguém tinha que lembrar, e agora é o merge que lembra

## 0.1.0 (2026-08-20)

- Primeira versão publicada por tag; o que veio antes está no histórico do git.
