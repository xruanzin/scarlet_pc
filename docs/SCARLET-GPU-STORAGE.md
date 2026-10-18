# Scarlet: dados de shaders e desenhos indiretos

Esta revisão parte de `ccb46f4` e do log `scarlet_boot(20261018-071703).log`.
Preserva as correções anteriores do boot, teclado, verificação do nome e imagens
de armazenamento. O código do jogo continua recompilado estaticamente; as
mudanças abaixo pertencem à camada NVN/Vulkan do host.

## O que o log comprova

O nome é aceito: `text-check result=0`, seguido do retorno da string UTF-8.
Depois aparecem quadros com cerca de 500 desenhos e compute ativo, muitas vezes
com `0 skipped`. Isso não comprova imagem correta: os desenhos podem receber
dados incorretos, e comandos ignorados não entravam no contador de skips.

Há gargalo real de GPU: no frame 1330, dos 2758 ms totais, 2747 ms aparecem como
espera de GPU, com `compile 0`. No final, há trechos perto de 4 FPS com centenas
de desenhos por quadro. Não é correto atribuir esse caso apenas ao teclado ou à
compilação de novos shaders.

## Memória global sem importação de memória do host

O caminho anterior de LDG/STG dependia de `VK_EXT_external_memory_host`. Sem essa
extensão, entregava um buffer de 4 KiB zerado ao shader. Qualquer shader que
precisasse dos buffers reais recebia dados ausentes. Isso pode ocultar geometria
e outros elementos mesmo quando o shader compila e o draw é enviado.

Agora, nesses dispositivos:

- O shader usa os buffers de armazenamento vinculados por NVN ao estágio.
- Um mapa com endereço e tamanho resolve os acessos de 8, 16, 32, 64 e 128 bits.
- Regiões sobrepostas compartilham o mesmo upload, preservando aliases.
- Escritas por shader são sincronizadas e devolvidas à memória do jogo antes
  dos consumidores seguintes: CPU, vértices, índices, uniforms ou texturas.
- Uploads transitórios são explicitamente publicados antes do submit, inclusive
  em tipos de memória que não sejam coerentes.

O projeto informa a ativação por linhas `[gmem] native NVN storage buffers`.
O método de usar os descritores de buffers NVN como fonte de endereço e tamanho
foi conferido no código fornecido do Citron:

- `src/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp`;
- `src/video_core/buffer_cache/buffer_cache.h`, `StorageBufferBinding`.

Limites: o fallback cobre os buffers de armazenamento vinculados por NVN; não
promete resolver endereços globais arbitrários sem um binding correspondente.
A devolução de escritas usa uma espera conservadora de GPU e ainda pode custar
desempenho. Dispositivos que anunciam importação continuam usando o caminho
anterior de importação. A execução completa no driver AMD do usuário não foi
testada neste ambiente.

## Desenhos indiretos

Os comandos abaixo eram gravados ou expostos, mas não executados no backend:

- `DrawArraysIndirect`;
- `DrawElementsIndirect`;
- `MultiDrawArraysIndirectCount`;
- `MultiDrawElementsIndirectCount`;
- `DispatchComputeIndirect`.

Agora são convertidos em operações executadas pelo backend, preservando a
quantidade de instâncias, primeiro índice, base de vértice assinada, base de
instância, limite de desenhos e stride. Contagens ou instâncias zeradas não
geram desenho. A leitura dos argumentos aguarda as escritas anteriores da GPU.
Isso remove comandos silenciosamente ignorados; o log existente não permite
afirmar sozinho quais deles causavam a tela vazia.

## Compilação e volume de log

Fragment e compute passam a usar a otimização de desempenho do shaderc. Vertex
mantém o modo anterior por padrão, preservando a invariância de profundidade
entre prepass e passe de cor. A opção de compilação faz parte da chave do cache.

`LARECOMP_VK_OPT=0` desativa a otimização. `LARECOMP_VK_OPT=1` também otimiza
vertex e continua sendo uma opção experimental. O primeiro uso desta revisão
pode recompilar módulos; o cache antigo não precisa ser apagado.

A alternância normal entre as duas texturas de apresentação não é mais escrita
no log a cada quadro. `LARECOMP_TRACE_PRESENT=1` restaura esse rastreamento.

## Validação feita

- Compilação dos arquivos C alterados com Clang/Zig 22 no Linux.
- 42 variantes LDG/STG compiladas por glslang 15.1 e validadas por SPIRV-Tools
  para Vulkan 1.3, nos estágios vertex, fragment e compute.
- 28 execuções numéricas no Vulkan Lavapipe: sete larguras/tipos de acesso,
  leitura/escrita, com e sem otimização. Os valores retornados foram conferidos,
  incluindo extensão de sinal e preservação de bytes vizinhos.
- Teste do agrupamento de aliases e do bloco real do executor para comandos
  indiretos, incluindo limite, stride, instâncias nulas e base de vértice negativa.
- Testes de modo de otimização e chave do cache, mais regressões de TLD4S e do
  sampler builder.

Os testes novos usam `-UNDEBUG`: seus asserts ficam ativos também com Zig em
compilação otimizada. Para executá-los:

```sh
python tools/test_storage_buffers.py
```

A validação de SPIR-V exige `glslangValidator` e `spirv-val`. A execução numérica
é opcional e exige `SCARLET_VULKAN_GPU_TEST=1`, headers e loader Vulkan. As
variáveis documentadas no início do teste permitem selecionar SDK e driver.
Sem essas dependências, os testes correspondentes são marcados como ignorados.

## Atualizar e testar no Windows

```powershell
git pull --ff-only origin scarlet-bootstrap
.\tools\scarlet_build_windows.ps1 -Jobs 4
.\tools\scarlet_run.ps1 -Profile
```

Não é necessário gerar novamente o `main` nem limpar a DLL do jogo. O teste
necessário agora é repetir a criação do personagem e a transição para a história,
conferindo a imagem e o novo `scarlet_boot.log`. Não houve validação visual dessas
cenas nem confirmação de gameplay neste ambiente.
