# Revisão de segurança

A seção 17 da SPEC lista quatro coisas que o sistema deve evitar.
Este documento diz, para cada uma, o que existe hoje, onde está a evidência, e o que ainda falta.

Achados abertos aparecem com severidade. Um achado sem correção fica aqui até ser corrigido, não some.

## 1. Comunicação de áudio ou vídeo sem criptografia

**Situação: coberto, e verificado por teste.**

Toda mídia atravessa DTLS-SRTP, que é o que WebRTC exige e o que as duas bibliotecas implementam: a libwebrtc no cliente, a libdatachannel no SFU.
Não existe caminho no código que negocie mídia em claro, porque nenhuma das duas oferece um.

A evidência não é a afirmação acima. `SfuTest.TheMediaIsEncryptedAndNothingElseIsOffered` lê o SDP que atravessa a negociação de verdade e exige três coisas de cada lado:

- `a=fingerprint:` presente, ou seja, o par DTLS é autenticado por certificado;
- perfil `RTP/SAVPF`, o seguro;
- ausência de `RTP/AVP`, que é o mesmo sem criptografia.

O signaling em si roda sobre WebSocket sem TLS na configuração padrão (`ws://`).
Em produção isso tem que ser `wss://`, e o cliente aceita ambos. **Achado, severidade média:** o padrão deveria ser recusar `ws://` para qualquer host que não seja loopback, em vez de aceitar em silêncio.

## 2. Armazenamento desnecessário de streams

**Situação: coberto.**

Nada no projeto grava áudio ou vídeo em disco.
Os quadros capturados vivem em uma fila de dois e são descartados; os decodificados vão para a tela e morrem.
O único caminho que escreve arquivo é o log, e ele registra números, não mídia.

## 3. Credenciais em texto puro

**Situação: parcialmente coberto, com um achado.**

O que já está certo:

- Senhas no servidor passam por scrypt com sal por conta, nunca são guardadas em claro (`server/src/signaling/authenticator.cpp`).
- A senha nunca é registrada em log. O hub registra o nome de usuário e diz isso explicitamente no código.
- `config::to_json` omite `turn_password` de propósito, para que despejar a configuração não vaze a credencial.
- O arquivo de contas de desenvolvimento é aceito, mas o servidor avisa em nível `warning` que ele contém senhas em texto puro e que precisa ser substituído antes de qualquer uso real.

**Achado corrigido nesta revisão: o hash de senha era SHA-256 com sal.**
Um digest é rápido por construção, que é exatamente a propriedade que um armazenamento de senha não pode ter: uma placa moderna testa bilhões de candidatos por segundo contra um arquivo roubado.

Agora é scrypt, com N de 2^14, r de 8 e p de 1, o que dá dezesseis mebibytes e algo como cinquenta milissegundos por tentativa.
Os parâmetros são deliberadamente os interativos e não os máximos: o mesmo custo que protege o arquivo roubado é o custo que uma enxurrada de logins aponta contra o servidor.

Dois testes guardam isso.
`HashingAPasswordIsDeliberatelySlow` falha se alguém trocar a derivação por um hash simples, porque o tempo cairia de milissegundos para microssegundos.
`TheSamePasswordStoresDifferentlyForDifferentUsers` garante que o sal por conta é usado, e não apenas gerado.

**Achado, severidade média: o cliente mantém a senha em memória durante toda a sessão.**
É consequência da reconexão automática do M7: o protocolo não tem retomada de sessão, então voltar depois que o servidor reinicia significa autenticar de novo.
A correção é um token de retomada emitido no `authenticated`, e está registrada em `docs/protocol.md`.

## 4. Tokens persistidos sem proteção

**Situação: coberto, por não existir persistência.**

O `authenticated` traz um token com validade, e o cliente o mantém apenas em memória.
Nada é escrito em disco, então não há nada para proteger em repouso.
Isso deixa de ser verdade no momento em que existir "lembrar de mim", e aí a resposta é o armazenamento de credenciais do sistema operacional, não um arquivo.

## 5. TURN com credenciais efêmeras

Pedido pela tarefa 6 do M8, e não coberto.

**Achado, severidade média: as credenciais de TURN são estáticas e vêm da configuração.**
Um usuário do aplicativo tem o usuário e a senha do servidor TURN, e pode usá-lo para o que quiser pelo tempo que quiser.

A correção conhecida é a de sempre com coturn: o servidor de signaling deriva `username = <expiração>:<user_id>` e `password = base64(HMAC-SHA1(segredo, username))`, e entrega isso ao cliente junto do `authenticated`.
O segredo nunca sai do servidor, e a credencial expira sozinha.
Isso é uma mudança de protocolo, então fica registrada aqui em vez de improvisada.

## 6. Um pacote que derrubava o servidor

Não pedido pela seção 17, e encontrado enquanto a tarefa 3 do M8 media adaptação de bitrate.

**Achado, severidade alta: qualquer participante autenticado derrubava o servidor inteiro com um pacote RTP.**

Um pacote com o bit de padding do RFC 3550 ligado, encaminhado pelo SFU, chega ao construtor de sender report da libdatachannel, que tem um `assert(!header->padding())`.
O processo aborta, e com ele todas as chamadas de todas as salas.

Não é preciso má fé para produzir um: a libwebrtc envia exatamente esses pacotes quando sonda banda, o que foi como isto apareceu.
Basta um cliente com uma extensão de cabeçalho a mais para que aconteça sozinho.

**Corrigido:** o SFU descarta pacotes com padding em vez de encaminhá-los.
Um participante que os envie perde os próprios quadros, que a retransmissão então repara, em vez de encerrar a chamada de todo mundo.
`tests/integration/test_sfu.cpp` envia pacotes com padding e exige que o servidor continue encaminhando vídeo depois; sem a correção, o teste aborta em vez de falhar.

O `assert` continua na libdatachannel, e continua sendo um `assert` em uma biblioteca de rede que processa entrada não confiável.
Enquanto estiver lá, nenhuma extensão que faça a libwebrtc sondar banda pode ser negociada.

## Resumo

| Item | Severidade | Situação |
| --- | --- | --- |
| Mídia sem criptografia | - | Coberto, verificado por teste |
| Armazenamento de streams | - | Coberto, nada é gravado |
| Hash de senha sem custo | Alta | Corrigido nesta revisão, scrypt |
| Pacote com padding derrubava o servidor | Alta | Corrigido, o SFU descarta o pacote |
| Senha em memória durante a sessão | Média | Aberto, precisa de token de retomada |
| Credenciais de TURN estáticas | Média | Aberto, precisa de credencial efêmera |
| Signaling sem TLS por padrão | Média | Aberto, deveria recusar `ws://` remoto |
| Tokens em repouso | - | Coberto, não há persistência |

O critério de aceitação do M8 é "nenhum achado aberto de alta severidade".
Não há nenhum aberto. Os três que restam são de severidade média e estão descritos acima com a correção conhecida de cada um.
