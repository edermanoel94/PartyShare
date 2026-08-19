# nvcodec

`nvEncodeAPI.h`, the header that declares NVIDIA's video encoder API.

| | |
| --- | --- |
| Origem | https://github.com/FFmpeg/nv-codec-headers, `include/ffnvcodec/nvEncodeAPI.h` |
| Versão da API | 13.1 |
| Licença | MIT, no cabeçalho do próprio arquivo |

## Por que está aqui

O NVENC não tem biblioteca para linkar: a `libnvidia-encode.so` vem com o driver e é aberta em tempo de execução, e o que o programa precisa em tempo de compilação é só a declaração das structs e dos enums que atravessam essa fronteira.

Essa declaração não é distribuída como pacote em todas as plataformas, e o projeto não pode depender de o desenvolvedor ter instalado o Video Codec SDK.
O ffmpeg resolve isso do mesmo jeito, e é de lá que este arquivo vem.

Nada é linkado: `client/src/webrtc/hardware_encoder_nvenc.cpp` abre `libnvidia-encode.so.1` e `libcuda.so.1` com `dlopen`, de forma que um binário compilado com NVENC roda igual em uma máquina sem placa NVIDIA nenhuma.

## Como atualizar

Baixe o arquivo de novo do repositório acima e atualize a versão da tabela.
As structs carregam um campo `version`, e o NVENC recusa uma versão mais nova do que o driver conhece, então subir isto sem necessidade só reduz o conjunto de máquinas em que o encoder funciona.
