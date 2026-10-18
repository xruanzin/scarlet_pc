# Scarlet: primeiro refinamento depois de obter imagem

O `scarlet_boot(20261018-035734).log` chega a apresentar quadros, alcança cerca
de 30 FPS em alguns trechos e termina com `window closed`. Isso confirma o
avanço do boot, mas não comprova que todas as cenas ou efeitos estejam corretos.

## Correção aplicada: TLD4S no Vulkan

Dois shaders de fragmento paravam na tradução das instruções abaixo:

| Instrução | PC no shader | Operação |
| --- | --- | --- |
| `df0200c181070f1a` | `0x728` | TLD4S, gather de textura |
| `df0000c0e3b73a0c` | `0x768` | TLD4S, gather de textura |

O tradutor agora gera `textureGather` ou `textureGatherOffset` no GLSL Vulkan.
Preserva o componente selecionado, a comparação de profundidade, os offsets
assinados de seis bits e a distribuição dos quatro resultados pelos dois pares
de registradores. A variante F16 empacota os resultados em pares de half.
Os operandos são lidos antes das escritas, inclusive quando os destinos são
também registradores de coordenadas. RZ continua descartando o resultado.

A codificação foi conferida com o
[tradutor TLD4S do Citron](https://github.com/citron-neo/emulator/blob/598b700884c7cd3bee0a25ce30d0cab066117c5b/src/shader_recompiler/frontend/maxwell/translate/impl/texture_gather_swizzled.cpp).
As chamadas GLSL seguem a
[especificação da Khronos](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html).
O dispositivo Vulkan habilita `shaderImageGatherExtended` quando disponível,
necessário para a variante com offset em registrador. Esta implementação de
TLD4S é específica do caminho GLSL; o caminho Metal ainda retorna erro explícito.

## Validado sem os arquivos do jogo

`tools/test_shader_gather.py` compila e executa o tradutor C real, usando shaders
sintéticos. Verifica as duas instruções exatas do log, metadados dos descritores,
componentes, coordenadas, profundidade, offsets, F16/F32, alias de registradores,
RZ e rejeição de formas inválidas. A revisão anterior reproduz os dois erros de
instrução desconhecida; a nova revisão passa.

Também foram compilados e validados 34 shaders sintéticos com glslang 15.1 e
SPIRV-Tools 2025.1 para Vulkan 1.3. Para repetir essa parte, deixe
`glslangValidator` e `spirv-val` no PATH, ou informe seus caminhos pelas variáveis
`GLSLANG_VALIDATOR` e `SPIRV_VAL`. Sem essas ferramentas, o teste de SPIR-V é
marcado como ignorado, não como aprovação.

```sh
python tools/test_shader_gather.py
```

Não houve execução do jogo ou medição de FPS no Windows nesta validação.
Outro opcode ainda não suportado pode aparecer depois do TLD4S no mesmo shader.

## Atualização no Windows

Na pasta do repositório, com a branch `scarlet-bootstrap` selecionada:

```powershell
git pull --ff-only
.\tools\scarlet_build_windows.ps1 -Jobs 4
.\tools\scarlet_run.ps1 -Profile
```

Não é necessário gerar novamente o `main` nem apagar caches para esta mudança.
O cache SPIR-V usa o conteúdo do GLSL, portanto os novos shaders recebem suas
próprias entradas. O build pode reaproveitar a DLL existente do código do jogo.

`-Profile` ativa `LARECOMP_VK_PROF=1` somente durante a execução e restaura o
valor anterior depois. O `scarlet_boot.log` passa a incluir linhas `[hitch]`
com tempo no backend, esperas de GPU, apresentação e tempo fora do backend.
Isso permite localizar as quedas de FPS que continuam mesmo com `compile 0`.
Esse modo serve para diagnóstico e pode acrescentar custo de instrumentação.

## Pendências observadas, ainda sem correção

- `eb20010600f70004` (`SUST`) e `eb50008602070a00` (`SURED`): operações de
  gravação/redução em imagens. Exigem tradução e descritores de storage image,
  além de sincronização correta no backend. Continuam falhando explicitamente.
- Um programa compute começa com instrução zero no endereço `727335c000`.
  É preciso investigar o programa e seu endereço antes de tratá-lo como vazio.
- Pipelines levam segundos para compilar em alguns trechos. Há também quedas
  de FPS com compilação já concluída; não se pode atribuir tudo ao cache.
- O áudio registra underruns. O serviço `hwopus` também tem comandos pendentes,
  mas o log sozinho não estabelece que isso seja a causa dos underruns.

O próximo teste útil é repetir a mesma cena com `-Profile`, registrar se os
elementos antes ausentes aparecem e comparar os erros e os tempos do novo log.
