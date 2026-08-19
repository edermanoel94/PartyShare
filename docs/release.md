# Release

Como uma versão sai, o que ela produz, e o que ainda não está verificado.

A regra é uma só: nenhum artefato é construído numa máquina de desenvolvimento.
Um binário publicado a partir de um laptop é um binário cujo conteúdo ninguém consegue reconstruir, e a primeira vez que isso importa é quando alguém reporta um crash em um build que não existe mais em lugar nenhum.

## Cortar uma versão

```sh
# 1. A versão vive em um lugar só.
$EDITOR CMakeLists.txt        # project(partyshare VERSION x.y.z)
$EDITOR vcpkg.json            # o mesmo número

# 2. Commit, tag, push.
git commit -am "Versão x.y.z"
git tag vx.y.z
git push origin master vx.y.z
```

A tag dispara `.github/workflows/release.yml`.
Nada além da tag é necessário: o workflow constrói, testa o que dá para testar, e publica.

Para exercitar o pipeline sem cortar uma versão, use `workflow_dispatch` pela interface do GitHub.
Ele constrói tudo e não publica nada, porque um release sem tag não tem versão para ser.

## O que a tag produz

| Artefato | Plataforma | Estado |
| --- | --- | --- |
| `partyshare-x.y.z-linux-x64.AppImage` | Linux x64 | Construído e verificado |
| `partyshare-x.y.z-windows-x64.zip` | Windows x64 | Escrito, nunca executado |
| `partyshare-x.y.z-macos-arm64.dmg` | macOS ARM64 | Escrito, nunca executado |
| `partyshare-x.y.z-macos-x64.dmg` | macOS x64 | Escrito, nunca executado |
| `SHA256SUMS` | - | Gerado a partir do que existir |

O `publish` roda com `always()` e exige só que o job do Linux tenha passado.
Um job de macOS que falha não pode segurar um artefato de Linux já construído e testado: parcial é um release pior que completo e muito melhor que nenhum.

## Linux

O único que esta máquina consegue verificar, e o único que está verificado.

`scripts/appimage.sh` faz o trabalho, e dá para rodar localmente:

```sh
scripts/appimage.sh                 # configura, compila, empacota
scripts/appimage.sh --skip-build    # só reempacota o que já está compilado
```

Três coisas que o script resolve e que não são óbvias:

- **A camada de mídia é obrigatória.**
  O primeiro AppImage construído subia sem ela: abria a janela, mostrava a tela de login, e não fazia chamada nenhuma.
  Isso é pior do que artefato nenhum, porque parece o produto.
  A ausência da libwebrtc agora é erro e não aviso, e o smoke test do CI recusa um artefato cujo log diga "no media layer".
- **O `strip` da ferramenta é velho demais.**
  O `linuxdeploy` carrega o próprio binutils, e ele rejeita a seção `.relr.dyn` que um linker atual emite, de forma fatal e não ignorável.
  O empacotamento acontece em duas passagens por isso, com o `strip` do sistema entre elas.
- **A glibc não pode ser embutida.**
  Qt e a runtime de C++ vão dentro; a glibc não vai.
  O AppImage roda em qualquer distribuição cuja glibc seja pelo menos tão nova quanto a da máquina que o construiu, e em nenhuma mais velha, e é por isso que o job usa o runner mais antigo disponível em vez do mais novo.

## Windows e macOS

Escritos a partir da documentação das ferramentas de cada plataforma, e **nunca executados**.
Este repositório é desenvolvido em Linux, e nenhum dos dois jobs jamais produziu um arquivo que alguém tenha instalado.

Trate a primeira execução como a coisa que vai descobrir o que está errado neles, não como uma regressão.
O que se sabe que falta, antes mesmo de rodar:

- O instalador do Windows é um ZIP do CPack com a runtime do Qt ao lado, não um `.msi` nem um `.exe` de instalação.
  A tarefa 1 do M9 pede um instalador de verdade, com entrada no menu iniciar e desinstalação.
- O `.dmg` é uma imagem com o bundle e um atalho para `/Applications`, sem plano de fundo nem posicionamento de ícones.
- Nenhum dos dois foi aberto em uma máquina limpa, que é o critério de aceitação do marco.

## Assinatura e notarização

Condicionais à existência dos segredos, e não presumidas.
Um fork ou uma primeira tag produzem artefatos sem assinatura em vez de um pipeline quebrado: um build sem assinatura é um build sobre o qual o sistema operacional reclama, e isso é uma falha melhor do que build nenhum.

| Segredo | Para que serve |
| --- | --- |
| `WINDOWS_CERTIFICATE` | Certificado de assinatura de código, `.pfx` em base64 |
| `WINDOWS_CERTIFICATE_PASSWORD` | Senha do `.pfx` |
| `MACOS_CERTIFICATE` | Certificado Developer ID Application, `.p12` em base64 |
| `MACOS_CERTIFICATE_PASSWORD` | Senha do `.p12` |
| `MACOS_SIGNING_IDENTITY` | Nome da identidade, como aparece no `security find-identity` |
| `MACOS_NOTARY_APPLE_ID` | Apple ID da conta de desenvolvedor |
| `MACOS_NOTARY_PASSWORD` | Senha específica de aplicativo, não a senha da conta |
| `MACOS_NOTARY_TEAM_ID` | Team ID de dez caracteres |

Nenhum deles está configurado hoje.
Notarização não é assinatura: a Apple quer o bundle assinado com um Developer ID, depois enviado, depois grampeado, e um bundle que pule qualquer uma das três é um que o Gatekeeper recusa a abrir em uma máquina que não o construiu.

## O que ainda não está feito

O M9 tem seis tarefas e nenhuma delas está fechada por completo.

- Instalador do Windows de verdade, no lugar do ZIP.
- `.dmg` com aparência, no lugar da imagem crua.
- Assinatura e notarização, que dependem de certificados que ninguém comprou.
- Instalar e rodar cada artefato em uma máquina limpa da respectiva plataforma, que é o critério de aceitação e não pode ser feito por quem só tem Linux.

O AppImage é o único que atravessa o caminho inteiro hoje, e mesmo ele não foi instalado em uma distribuição diferente da que o construiu.
