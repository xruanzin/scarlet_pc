# Scarlet: comandos de GPU preservados e entrada na seleção de personagem

Análise do log `scarlet_boot(20261018-125740).log`, sobre a revisão `25afbcd`.
O processo chegou a apresentar mais de 500 quadros e terminou com `window closed`.
Não há crash registrado. Isso não comprova que a lógica da tela estivesse avançando.

## Evidências e correções

### Comandos descartados durante a compilação

O primeiro present foi solicitado após 10,538 s. Depois, os intervalos registraram
até 960 desenhos descartados, por programas ainda compilando ou pipelines em construção.
Alguns builds de pipelines levaram aproximadamente 9,7 s. A linha antiga
`compiles N pending` contava builds **concluídos** desde o último relatório;
não representava trabalhos pendentes.

O backend aguardava apenas 4 ms por quadro nessa CPU e descartava o restante dos
desenhos ainda sem pipeline. Compute também podia ser descartado no primeiro uso.
Isso é incorreto para operações que alimentam caches, imagens e buffers persistentes:
o jogo pode considerar o comando enviado e nunca repeti-lo. Uma escrita perdida
no atlas de letras é uma explicação possível para texto ausente após a compilação,
mas este log não identifica o conteúdo de cada desenho.

Agora programas, pipelines e compute aguardam a conclusão antes de executar o
comando original, preservando sua ordem. Falhas reais continuam registradas.
`LARECOMP_SHADER_WAIT_MS` deixa de descartar comandos. A fila continua usando
workers; se nenhum puder ser criado, o trabalho executa na própria thread.
Não há apresentação Vulkan concorrente dentro da espera: eventos SDL continuam
na thread de interface existente.

Esperas acima de um segundo registram `[shader-wait]`, o tipo de build, endereços,
tempo e a quantidade real de trabalhos pendentes. **A primeira visita ainda pode
parar para compilar.** Preservar comandos não elimina o custo do driver. Os caches
SPIR-V e de pipelines continuam ativos; não apague a pasta `cache` para esta atualização.
Não foi medido ganho de tempo de abertura no computador do usuário.

### Imagens de armazenamento

A montagem do mapa de imagens chamava `vk_tex(t, true)` antes de verificar se a
vista era compatível. Essa chamada marcava a textura como produzida pela GPU,
inclusive quando a vista era recusada, impedindo o upload inicial da CPU.
Operações atômicas também podiam receber uma imagem sem seu valor inicial.

Agora a verificação de formato não altera a propriedade dos dados. Imagens
compatíveis recebem seus dados iniciais e atualizações da CPU; as imagens que
podem ser escritas pelo shader passam a ser consideradas da GPU após o comando
ser efetivamente registrado. Uma vista rejeitada não muda essa propriedade.
Como o handle é dinâmico, a marcação após o comando é conservadora e abrange os
slots compatíveis vinculados ao shader.
Ao finalizar uma textura, seus descritores são removidos e a geração do pool
avança, para o mapa seguinte não reutilizar referências a objetos já liberados.

O log mostrava mapas com `write=0 atomic=1`: havia uma vista R32_UINT, sem vista
float compatível. O SUST 2D sem formato explícito anteriormente só escrevia na
lista float. Agora também consegue escrever nessa vista R32_UINT, preservando
os bits inteiros dos registradores. O log não demonstra qual TIC cada SUST usava.
O suporte adicionado é para R32_UINT; outros formatos inteiros e operações de
surface ainda não implementadas não passam a funcionar por consequência.

A semântica foi conferida em `surface_load_store.cpp` e nos tipos de imagem do
código do Citron fornecido pelo usuário. Isso permanece tradução NVN/Vulkan
com CPU recompilada estaticamente, sem adicionar intérprete ou JIT da CPU do console.

### Leitura do controle em lote

Havia adaptadores de `GetNpadState`, mas não de `GetNpadStates`. A implementação
original em lote depende dos anéis de memória compartilhada HID, que o backend
de entrada do host não preenche. Foram adicionados os cinco adaptadores em lote,
com a ABI `(states, capacidade, id)` e retorno de uma amostra atual válida quando
a capacidade é positiva. Capacidades zero/negativas não escrevem no destino.
Um ID desconectado recebe estado desconectado, e o menu continua capturando a entrada.
Isso cobre chamadas importadas desses símbolos; leituras diretas da memória HID
continuam fora desse caminho. O log anterior não comprova uso dessas funções na tela.

Perder o foco da janela também limpa teclas pressionadas, evitando teclas presas
quando o Windows entrega o evento de soltura a outra janela.

Com `-Profile`, `[input-state]` registra botões do host, entrada liberada pelo menu,
captura e contagens de leituras. Mudanças dos botões entregues ao jogo também
aparecem, inclusive pressões curtas entre os relatórios. `LARECOMP_INPUT_TRACE=1`
ativa esse diagnóstico sem perfil de GPU. Esses dados ajudam a separar entrada
capturada, ausência de polling e espera por compilação.

## Validação

- Teste da função real de espera: builds atrasados em 40 ms preservam operações
  únicas de programa, pipeline e compute, além de reconhecer falha terminal.
- Teste dos cinco adaptadores Npad reais, capacidades 0/1/16/negativa, preservação
  do destino seguinte, ordem das amostras, ID desconectado e captura pelo menu.
- Testes das funções reais de vistas e da marcação posterior: vistas incompatíveis
  não alteram a propriedade da textura, e a inicialização antecede escritas parciais.
- Teste da remoção de descritores de texturas finalizadas e da invalidação do pool.
- Seis shaders SUST/SURED traduzidos, compilados e validados nos estágios vertex,
  fragment e compute.
- Seis execuções numéricas Vulkan: RGBA8, R32_UINT e soma atômica, cada um com e
  sem otimização. Conferidos o pixel escrito e os pixels vizinhos preservados.
- Regressões LDG/STG e TLD4S e compilação dos arquivos C alterados no Linux.

Os testes numéricos usam Vulkan Lavapipe. Não equivalem a executar Scarlet no driver
AMD do Windows. Não há NSOs/RomFS do jogo neste ambiente para verificar a seleção
de personagem, todos os textos ou o início da história.

```sh
python tools/test_scene_progress.py
```

Os testes precisam de Clang/Zig e headers SDL3/Vulkan. `SDL_INCLUDE` e
`VULKAN_INCLUDE` selecionam os diretórios. A validação usa `GLSLANG_VALIDATOR`
e `SPIRV_VAL`; a execução numérica é ativada com `SCARLET_VULKAN_GPU_TEST=1`,
`VULKAN_LIBRARY` e, se necessário, `VK_ICD_FILENAMES`. `SPIRV_OPT` permite testar
também o shader otimizado. Dependências ausentes de GPU são informadas como teste
ignorado, sem serem contadas como execução bem-sucedida.

## Atualização no Windows

```powershell
git pull --ff-only origin scarlet-bootstrap
.\tools\scarlet_build_windows.ps1 -Jobs 4
.\tools\scarlet_run.ps1 -Profile
```

Não é necessário regenerar a DLL do jogo por estas alterações de runtime.
Depois da compilação inicial de shaders, teste movimento e confirmação na seleção
de personagem e observe se os textos aparecem. Para comparar a abertura com cache
já gravado, feche normalmente e execute novamente. Se continuar sem resposta,
o novo `scarlet_boot.log` passa a distinguir as hipóteses descritas acima.
