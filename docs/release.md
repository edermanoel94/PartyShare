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
| `partyshare-x.y.z-windows-x64.exe` | Windows x64 | Escrito, nunca executado |
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
- **A glibc não pode ser embutida, e isso decide onde o artefato roda.**
  Qt e a runtime de C++ vão dentro; a glibc não vai.
  O AppImage roda em qualquer distribuição cuja glibc seja pelo menos tão nova quanto a da máquina que o construiu, e em nenhuma mais velha.

  Isso não é teórico. Um AppImage construído nesta máquina de desenvolvimento, que roda Arch com glibc 2.44, **não inicia em um Ubuntu 24.04 limpo**: pede `GLIBC_2.43` e `GLIBC_2.44`, e o 24.04 tem 2.39.
  O script imprime o piso do arquivo que acabou de produzir por isso, porque é uma propriedade invisível até alguém não conseguir abrir o programa.

  Um AppImage construído localmente é artefato de desenvolvimento, não de distribuição. O distribuível sai do CI, que constrói no runner mais antigo disponível.

### Do que o AppImage depende do sistema

Nem tudo é embutido, e o que fica de fora é deliberado: embutir a fontconfig faz o programa parar de achar as fontes do sistema, e embutir a pilha gráfica faz ele parar de achar o driver da máquina.

O conjunto exato, verificado com `ldd` sobre a árvore extraída em um container limpo:

```text
libX11  libX11-xcb  libxcb  libICE  libSM
libEGL  libGLX  libOpenGL  libdrm  libgbm
libfontconfig  libfreetype  libharfbuzz
```

Todo desktop Linux já tem os treze.
O teste de máquina limpa do workflow instala exatamente essa lista e nada mais, o que é o que a mantém honesta: uma dependência nova que entrar sem estar aqui falha ali.

## Windows e macOS

Escritos a partir da documentação das ferramentas de cada plataforma, e **nunca executados**.
Este repositório é desenvolvido em Linux, e nenhum dos dois jobs jamais produziu um arquivo que alguém tenha instalado.

Trate a primeira execução como a coisa que vai descobrir o que está errado neles, não como uma regressão.
O que eles pretendem produzir:

- O instalador do Windows é NSIS, com atalho no menu iniciar, ícone próprio e desinstalação. O ZIP continua ao lado para quem prefere os arquivos sem um instalador mexendo na máquina.
- O `.dmg` tem o bundle, o atalho para `/Applications` e ícone de volume. Falta plano de fundo e posicionamento de ícones na janela, que é aparência e não função.
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

Das seis tarefas do M9, duas estão fechadas: o AppImage e a publicação por tag.

- Rodar os jobs de Windows e macOS uma primeira vez, que é o que vai dizer o que está errado neles.
- `.dmg` com plano de fundo e posicionamento de ícones.
- Assinatura e notarização, que dependem de certificados que ninguém comprou.
- Instalar e rodar o artefato de Windows e o de macOS em uma máquina limpa da respectiva plataforma.

O Linux é o único que atravessa o caminho inteiro, e o teste de máquina limpa do workflow é o que sustenta essa afirmação: o job constrói no Ubuntu 22.04 e inicia o resultado em um container de Ubuntu 24.04 com só as treze bibliotecas de sistema listadas acima.
