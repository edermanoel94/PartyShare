# Protocolo de signaling

Este documento é a definição normativa do protocolo.
A implementação em C++ em `shared/include/dv/protocol/message.hpp` segue este documento, e não o contrário.
Isso permite reimplementar o servidor em outra linguagem sem alterar os clientes, como previsto na seção 14 da SPEC.

## 1. Transporte

O transporte é WebSocket, com uma mensagem JSON por frame de texto.
Frames binários não são usados nesta versão.

O cliente conecta em `ws://` ou `wss://`.
Fora de desenvolvimento local, apenas `wss://` deve ser aceito, porque o token de sessão trafega no canal.

## 2. Envelope

Toda mensagem é um objeto JSON plano com um campo discriminador `type`.

```json
{ "type": "join_room", "room_id": "8F42A1", "user_id": "user123" }
```

Regras:

- `type` é obrigatório e precisa ser uma string conhecida.
- Campos desconhecidos são ignorados pelo receptor.
  Isso é intencional: permite acrescentar campos opcionais sem quebrar clientes antigos.
- Um campo presente com valor `null` é tratado como ausente.
- Campos obrigatórios ausentes são erro.

## 3. Identificadores

`room_id` tem exatamente 6 caracteres hexadecimais maiúsculos, por exemplo `8F42A1`.
Isso dá 16.777.216 combinações, suficiente para o MVP, e é curto o bastante para ser ditado por voz.

`user_id` é uma string opaca atribuída pelo servidor na autenticação.
Clientes nunca devem inferir significado a partir dela.

O identificador `sfu` é reservado e não pertence a nenhuma pessoa.
Ele representa o ponto de mídia do próprio servidor, descrito na seção 4.3.
Como os identificadores atribuídos pelo servidor têm 16 caracteres hexadecimais, não há como um participante recebê-lo por acidente.

## 4. Mensagens

### 4.1 Cliente para servidor

| Tipo | Campos obrigatórios | Campos opcionais |
| --- | --- | --- |
| `authenticate` | `username`, `password` | |
| `create_room` | `user_id` | `room_name` |
| `join_room` | `room_id`, `user_id` | `display_name` |
| `leave_room` | `room_id`, `user_id` | |

`authenticate` precisa ser a primeira mensagem da conexão.
Qualquer outra antes dela é respondida com `error` de código `unauthorized`.

A senha só aparece nessa mensagem.
O servidor nunca a repete de volta e nunca a grava em log.

### 4.2 Servidor para cliente

| Tipo | Campos obrigatórios | Campos opcionais |
| --- | --- | --- |
| `authenticated` | `user`, `token`, `expires_in_seconds` | |
| `room_created` | `room_id` | `room_name` |
| `user_joined` | `room_id`, `user` | |
| `user_left` | `room_id`, `user_id` | |
| `error` | `code` | `message` |

O objeto `user` tem a forma:

```json
{ "id": "user123", "display_name": "Ana", "avatar": "" }
```

`id` e `display_name` são obrigatórios, `avatar` é opcional.

### 4.3 Negociação WebRTC

| Tipo | Campos obrigatórios |
| --- | --- |
| `offer` | `room_id`, `from_user_id`, `to_user_id`, `sdp` |
| `answer` | `room_id`, `from_user_id`, `to_user_id`, `sdp` |
| `ice_candidate` | `room_id`, `from_user_id`, `to_user_id`, `candidate`, `sdp_mid`, `sdp_mline_index` |

`sdp_mline_index` é um inteiro.

O servidor deve validar que `from_user_id` corresponde à conexão que enviou a mensagem.
Aceitar um `from_user_id` arbitrário permitiria a um participante se passar por outro.

Essas três mensagens têm dois destinos possíveis, decididos por `to_user_id`.

**Para outro participante.**
O servidor não interpreta SDP nem candidatos, apenas encaminha a mensagem inalterada para `to_user_id` dentro da mesma sala.

**Para `sfu`.**
A mídia do MVP passa por um SFU, então a outra ponta da conexão de cada participante é o servidor, e não outro participante.
Uma mensagem endereçada a `sfu` é consumida pelo servidor, e o que ele devolve chega com `from_user_id` igual a `sfu`.

Nesse caminho quem oferece é sempre o servidor:

```text
servidor -> participante:  offer          assim que o participante entra na sala
participante -> servidor:  answer
ambos os sentidos:         ice_candidate
```

O servidor reoferece sempre que o conjunto de participantes muda, acrescentando ou removendo uma linha de mídia.
Cada linha `sendonly` de áudio que o servidor oferece traz `a=msid:<user_id>`, e é assim que o cliente sabe de quem é a voz que chega naquela track.

Cada participante recebe também duas linhas de vídeo, criadas junto com a sessão e não quando alguém pede para compartilhar:

```text
recvonly, H.264   a tela do próprio participante, subindo
sendonly, H.264   a tela de quem estiver compartilhando, descendo
```

Elas existem desde a entrada na sala, vazias, porque assim começar e parar de compartilhar não renegocia nada.

A linha de vídeo `sendonly` não tem `a=msid:<user_id>`: ela carrega quem estiver com a palavra, e não um participante fixo.
Quem está compartilhando é dito por `screen_share_started`, e é de lá que o cliente tira o nome.

O participante que envia mídia precisa declarar seu SSRC no `answer`, com `a=ssrc`, em cada linha que ele envia.
Todas as tracks compartilham um transporte, então é o SSRC que diz ao servidor a qual track pertence cada pacote que chega.
Sem ele a mídia é descartada silenciosamente na chegada.

Pedidos de keyframe atravessam o SFU. Um espectador que precisa de um quadro intra manda PLI na track em que recebe, e o servidor repassa o pedido para as tracks de vídeo que sobem naquela sala, porque nada no meio decodifica o vídeo para produzir um.

Um servidor sem roteamento de mídia responde `media_unavailable` a qualquer mensagem endereçada a `sfu`.

### 4.3.1 Reconexão

O cliente reconecta sozinho quando a conexão cai, com o intervalo dobrando a cada tentativa até um teto.
Só um pedido explícito de desconexão encerra isso.

Reconectar é começar do zero do ponto de vista do protocolo: uma conexão nova, um `authenticate` novo e um `join_room` novo, na mesma sala.
Não existe retomada de sessão, e a identidade do usuário é reemitida pelo servidor como em qualquer outro login.
Quem já estava na sala vê o participante sair e entrar de novo.

### 4.4 Mudanças de estado

| Tipo | Campos obrigatórios |
| --- | --- |
| `screen_share_started` | `room_id`, `user_id` |
| `screen_share_stopped` | `room_id`, `user_id` |
| `mute` | `room_id`, `user_id` |
| `unmute` | `room_id`, `user_id` |

Essas quatro mensagens trafegam nos dois sentidos.
Do cliente para o servidor, são um pedido.
Do servidor para os clientes, são a confirmação, retransmitida para todos os outros participantes da sala.

Um cliente só deve atualizar sua própria UI após receber a confirmação do servidor, e não ao enviar o pedido.
Caso contrário duas pessoas podem acreditar simultaneamente que estão compartilhando a tela.

### 4.5 Nível de transporte

| Tipo | Campos obrigatórios | Campos opcionais |
| --- | --- | --- |
| `ping` | | `nonce` |
| `pong` | | `nonce` |

O servidor envia `ping` a cada `heartbeat_interval_ms`.
O cliente responde `pong` com o mesmo `nonce`.
Um cliente que não responde dentro de `heartbeat_timeout_ms` é considerado desconectado e removido da sala.

## 5. Códigos de erro

Códigos são estáveis e podem ser comparados por igualdade.
A mensagem que os acompanha é apenas para humanos e pode mudar.

| Código | Significado |
| --- | --- |
| `invalid_json` | O payload não é JSON válido, ou não é um objeto |
| `missing_field` | Um campo obrigatório está ausente |
| `invalid_type` | Um campo existe com o tipo JSON errado |
| `unknown_message_type` | O `type` não pertence a este protocolo |
| `room_not_found` | Não existe sala com esse `room_id` |
| `room_full` | A sala já atingiu o limite de participantes |
| `already_in_room` | O usuário já está na sala |
| `not_in_room` | A operação exige que o usuário esteja na sala |
| `screen_share_busy` | Outro participante já está compartilhando a tela |
| `unauthorized` | Token de sessão ausente, inválido ou expirado |
| `media_unavailable` | A mensagem foi endereçada a `sfu`, e este servidor não roteia mídia |

Os cinco primeiros são detectados na camada de parsing e estão implementados desde o M1.
Os seguintes dependem do servidor e chegaram no M2, e `media_unavailable` no M4.

## 6. Máquina de estados da sessão

```text
        ┌──────────────┐
        │ Disconnected │
        └──────┬───────┘
               │ conexão WebSocket estabelecida
               ▼
        ┌──────────────┐
        │  Connected   │
        └──────┬───────┘
               │ authenticate aceito
               ▼
        ┌──────────────┐
        │Authenticated │
        └──────┬───────┘
               │ create_room / join_room
               ▼
        ┌──────────────┐   error (room_full, room_not_found)
        │   Joining    │ ─────────────────────────────────────┐
        └──────┬───────┘                                      │
               │ user_joined referente a si mesmo             │
               ▼                                              │
        ┌──────────────┐                                      │
        │   InRoom     │                                      │
        └──────┬───────┘                                      │
               │ leave_room, queda de conexão                 │
               ▼                                              ▼
        ┌──────────────┐                              ┌──────────────┐
        │  Connected   │                              │  Connected   │
        └──────────────┘                              └──────────────┘
```

Transições relevantes:

- Em `Connected`, apenas `authenticate` é aceito.
  Toda outra mensagem recebe `error` com código `unauthorized`.
- Em `Authenticated`, apenas `create_room` e `join_room` são aceitos.
- Em `Joining`, o cliente aguarda `user_joined` ou `error`.
- Em `InRoom`, todas as mensagens de negociação e de estado são aceitas.
- Uma queda de conexão em qualquer estado leva a `Disconnected`.
  O cliente reconecta com backoff exponencial e precisa refazer `authenticate` e depois `join_room`.
  O servidor não preserva o estado da sessão entre conexões no MVP.

## 7. Ordem das mensagens ao entrar em uma sala

Quando o usuário C entra em uma sala que já contém A e B, o servidor envia:

```text
para C:      user_joined (A)
para C:      user_joined (B)
para C:      user_joined (C)      <- confirma a própria entrada, sempre por último
para A e B:  user_joined (C)
```

O `user_joined` referente ao próprio usuário chega por último e é o sinal de que o estado inicial está completo.
Isso evita que o cliente precise de uma mensagem de snapshot separada.

Se algum participante estiver compartilhando a tela, o servidor envia `screen_share_started` para C logo após a sequência acima.

Depois disso, com roteamento de mídia ligado, vem a negociação com o SFU descrita na seção 4.3:

```text
para C:      offer (from_user_id sfu)
para A e B:  offer (from_user_id sfu)     reoferta, agora com a track de C
```

A ordem importa: o `offer` chega depois do `user_joined`, então o cliente já conhece todos os participantes quando precisa associar uma track a alguém.

## 8. Compatibilidade

Acrescentar um novo campo opcional é compatível com versões anteriores.
Acrescentar um novo tipo de mensagem é compatível: receptores antigos respondem `unknown_message_type` e continuam funcionando.

Não são compatíveis, e exigem versionamento explícito do protocolo:

- Remover ou renomear um campo obrigatório.
- Mudar o tipo JSON de um campo.
- Mudar o significado de um código de erro existente.
