# Correção da regressão de abertura e resposta do Windows

## Shader real recebido após o log 155335

O shader enviado tem 1850 instruções/casos no dispatcher e dois BRX. Ambos
usam o padrão `IMNMX.U32 -> SHL 2 -> LDC c1 -> BRX`, com três entradas por tabela,
nos offsets 0 e 12 das constantes do programa. O fallback anterior tratava toda
instrução como destino, criando 1850 entradas no switch. Há somente 32 destinos
diretos/de reconvergência; os destinos adicionais devem vir das tabelas reais.

O backend agora fornece ao tradutor a seção de constantes imutáveis CB1 do
mesmo estágio NVN, tanto em graphics quanto em compute. A análise só reduz
rótulos quando prova definições adjacentes e incondicionais, mínimo unsigned,
shift de dois bits, LDC de 32 bits em CB1 e tabela limitada à seção válida.
Entradas desalinhadas/fora do código, tabelas truncadas, predicação, outros
buffers e saltos para o interior da sequência mantêm o dispatcher completo.
As instruções e as leituras de CB1 continuam executando com os mesmos valores;
não há seleção artificial de uma variante visual ou omissão de um draw.

`[shader-flow]` informa instruções, casos, tabelas resolvidas e fallbacks.
A implementação foi escrita a partir do padrão observado e conferida com a
análise de tabelas do [Citron](https://github.com/citron-neo/emulator/blob/main/src/shader_recompiler/frontend/maxwell/indirect_branch_table_track.cpp).
A recompilação estática da CPU do jogo permanece igual.

Também substituímos os wrappers `as_f/as_u` por intrínsecos GLSL. Os atributos
inteiros de vértice usam conversão explícita para uint; os atributos float usam
bitcast. No shader enviado, essa mudança isolada reduz o SPIR-V de **140537 para
87275 palavras**, antes de reduzir os rótulos. As chamadas de função no módulo
caem de 4649 para 216. O código de vértice mantém
`NoContraction`. O modo shaderc continua sem otimização agressiva por padrão.

O ZIP enviado continha somente o log. O log mostra que a pasta de dados real
é `.../scarlet_pc/build`; o runtime muda o diretório do processo para lá. Por
isso, `build/shader-diagnostics` é relativo à pasta de dados, e não à raiz do
repositório. A coleta do ZIP agora lê `[paths] data:` e resolve os arquivos a
partir dessa pasta. Saves e caches existentes não são movidos. A coleta também é executada ao fechar
uma sessão que registrou `[shader-slow]`, mesmo quando o processo retorna zero.

Validação: duas tabelas na mesma seção, comparação do corpo GLSL compacto e do
fallback completo, 13 formas de prova inválida, destinos em palavras de controle,
MSL idêntico entre APIs e tipos de atributos de vértice. Doze renderizações
Vulkan conferem todos os índices, saturação e wrap unsigned, nos dois dispatchers,
com e sem a flag rápida de pipeline. Regressões de surface/storage/gather e
compilação Windows x64 também são verificadas. O teste PowerShell reproduziu o
ZIP sem shaders antes da correção e passou com a pasta real de dados depois.

Os arquivos enviados não incluem as constantes binárias CB1 nem o shader Maxwell
original; o número exato de casos do jogo será confirmado por `[shader-flow]`
na próxima execução. Testes locais usam Lavapipe. Não há medição do boot completo
nem confirmação do comportamento do driver AMD proprietário neste ambiente.

## Política atual de espera, integrada da branch

Enquanto esta correção era validada, a branch recebeu `2ac3841`. Essa mudança
foi integrada sem ser revertida: o padrão passa a esperar 30 s por build e depois
adiar/omitir operações que ainda dependem dele, mantendo a compilação em segundo
plano. São dois workers até 12 CPUs lógicas e três nos demais computadores.
`LARECOMP_SHADER_WAIT_MS` configura a espera. `LARECOMP_SHADER_TIMEOUT_EXIT=1`
reativa a saída 86 ao atingir o prazo; os testes cobrem ambos os modos.

Essa política não é replay de comandos: operações que só ocorrem uma vez podem
ser perdidas caso o build ultrapasse o prazo. A correção deste documento atua no
custo de compilação e no grafo do shader; não declara que a omissão de operações
resolve textos/atlas. O novo boot AMD e eventuais `[shader-slow]` precisam ser
conferidos. Os logs de saída 86 a seguir são das revisões anteriores.

## Resultado do log 153427 e correção do compilador

O log `scarlet_boot(20261018-153427).log`, da revisão `1681e95`, chegou ao
Vulkan/NVN em **2,650 s**. A proteção de memória foi instalada (7155 MiB).
Depois, o programa `72730b5000/72730f3000` levou **6,1 s** para gerar SPIR-V:
830 palavras no vertex e **364040 palavras no fragment**. O pipeline seguinte
atingiu o prazo de 8 s e terminou com código 86. A proteção funcionou, mas o
primeiro quadro ainda não foi apresentado.

A revisão `25afbcd` tinha ativado a otimização de desempenho do shaderc por
padrão em fragment/compute. Ela foi revertida para o modo original do port:
**sem essa otimização em todas as etapas**. `LARECOMP_VK_OPT=1` continua disponível
como experimento ao executar o binário diretamente. O script normal força `0`
durante o processo e restaura o ambiente ao terminar. O driver continua gerando
o código nativo da GPU; isso não muda a recompilação estática da CPU do jogo.

O teste com o compilador shaderc real reproduz um problema desse tipo: um shader
sintético com 128 registradores e 1024 blocos de controle cresce para **307901
palavras** com a otimização, contra **66134** no modo restaurado. Uma execução
mediu **1,157 s** e **0,097 s**, respectivamente. A promoção dos registradores
para SSA cria grandes nós Phi no dispatcher. São dados do teste sintético,
não medidas do shader do jogo ou do driver AMD do usuário. O GLSL desse shader
específico ainda não estava no log; portanto a causa exata de suas 364040
palavras não está confirmada.

A opção de compilação faz parte da chave do cache. Um módulo antigo otimizado
não é usado pelo novo modo padrão. Não apague o cache: os módulos compatíveis
continuam aproveitáveis. A precisão de vertex (`NoContraction`) e as correções
de storage, textos/atlas e comandos de primeiro uso são preservadas.

Shaders com fonte de pelo menos 64 KiB ou módulo de pelo menos 32768 palavras
salvam automaticamente o GLSL e o SPIR-V em `build/shader-diagnostics`. A fonte
grande é salva antes de chamar shaderc, inclusive se a compilação não terminar.
Ao haver saída 86 ou `[shader-slow]` no log ao fechar o jogo, `scarlet_run.ps1`
cria **`build/scarlet_shader_failure.zip`** com o log atual e os arquivos de
shader mencionados nele. Esse arquivo permite
investigar a entrada real caso a reversão ainda não resolva o pipeline na AMD.

## Evidência anterior

O log `scarlet_boot(20261018-150324).log`, da revisão `46a24f4`, registra
`Vulkan/NVN device ready after 2.631 s`. Depois fica pelo menos **119 s**
esperando um único pipeline (`72730b4000/72730f2000`), com `jobs=1`, sem chegar
ao primeiro present. Já o log `071703` registrava o primeiro present em 4,243 s.
Portanto, a regressão de abertura está no primeiro pipeline e na espera sem prazo
introduzida na revisão anterior. Não é correto atribuí-la ao teclado ou ao
carregamento normal da história.

O usuário relatou perda de resposta do computador inteiro. O log não registra
consumo de RAM, paginação ou um relatório de falha do driver; não permite afirmar
sozinho qual desses mecanismos travou o Windows. As mudanças abaixo corrigem a
espera e reduzem o trabalho e a pressão de recursos associados à compilação.

## Compilação e espera das revisões 1681e95 / 96aeda8

- O laço que despacha o controle de fluxo dos shaders GLSL recebe `[[dont_unroll]]`.
  O limite de 100000 iterações é uma guarda de execução, e não deve ser expandido
  em cópias do corpo durante otimização. O teste verifica `DontUnroll` no SPIR-V.
  O log não prova que a expansão desse laço causou o bloqueio específico da AMD.
- Pipelines gráficos e compute traduzidos usam
  `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` na AMD. Isso solicita menos
  otimização ao driver na criação; as regras de precisão dos shaders
  de vértice permanecem como antes. O modo shaderc é o descrito acima. Não
  substitui nem omite o shader original.
  O efeito no desempenho da gameplay ainda precisa ser medido no Windows.
- Até 12 CPUs lógicas, há um worker de compilação em baixa prioridade; nos demais,
  dois. Essa redução controla concorrência futura: o log problemático já tinha
  somente um trabalho pendente, portanto ela sozinha não explica uma solução.
- A espera por programa, pipeline ou compute tem prazo de **8 s**. Se exceder,
  grava `[build-timeout]` e termina o processo com código **86**, sem tentar
  destruir o dispositivo Vulkan ou esperar o cache do compilador.
- Fechar a janela enquanto há compilação ativa também encerra sem esperar o
  cache. Operações que terminam dentro do prazo continuam sendo preservadas.

**O prazo é um encerramento por falha, não uma correção visual nem garantia de
abertura em oito segundos.** Uma compilação ainda patológica deve encerrar o
processo, em vez de repetir a espera de dois minutos. O prazo abrange as esperas
dos trabalhos assíncronos traduzidos, não todas as chamadas possíveis ao driver.
O modo de teste `LARECOMP_SYNC_SHADERS` executa builds inline e não deve ser usado
para repetir este caso de bloqueio.

## Recursos no Windows

- O processo inicia em prioridade **normal**. Pedidos FastLoad do jogo mantêm essa
  classe, e a thread de temporização deixa de usar `TIME_CRITICAL`.
- Um Job Object limita memória privada comprometida: o menor valor entre
  8 GiB, 60% da memória física e 75% da memória disponível na abertura.
- Não há limite artificial de working set, nem bloqueio da reserva de endereços
  do jogo ou do mapeamento de arquivo RomFS. O limite privado não representa toda
  a RAM/VRAM usada pela GPU ou por outros processos. Ao atingir o limite, uma
  alocação pode falhar e o jogo pode encerrar.
- O log informa o limite efetivamente instalado. Se o Windows recusar a associação
  ao Job Object, registra `process commit limit unavailable`; não declara proteção
  ativa nesse caso.

Referências primárias:
[Vulkan: flags de criação de pipelines](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreateFlagBits.html),
[Microsoft: prioridade do processo](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass),
[Microsoft: limite de memória comprometida](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information).

## Cache e execução

O script inicia a partir da raiz do repositório, inclusive quando chamado por
caminho absoluto. O runtime escolhe a pasta de dados e a informa em `[paths] data:`;
com o jogo extraído em outra unidade, ela normalmente é a pasta do executável,
`build`. O script usa essa informação para reunir os diagnósticos corretamente.
Não apague nem mova saves/caches para atualizar. Shaders cujo GLSL mudou recebem
outra chave; os demais continuam aproveitáveis.

```powershell
cd "C:\Users\ruana\Downloads\scarlet_pc"
git pull --ff-only origin scarlet-bootstrap
if ($LASTEXITCODE -ne 0) { throw "Falha ao atualizar o repositorio." }
Set-ExecutionPolicy -Scope Process Bypass
.\tools\scarlet_build_windows.ps1 -Jobs 4
if ($LASTEXITCODE -ne 0) { throw "Falha na compilacao." }
.\tools\scarlet_run.ps1
```

O teste inicial usa o perfil de GPU desligado. As linhas de boot e de espera
continuam no `scarlet_boot.log`. Se houver `[shader-slow]` ou saída 86 no modo
de diagnóstico, preserve o log e `build/scarlet_shader_failure.zip`.
Não é necessário regenerar a DLL do jogo para estas mudanças de runtime.

## Validação e limites

- Quatro novos testes com a implementação C real e a biblioteca shaderc: expansão
  do dispatcher, cache separado entre modos, diagnóstico de fonte antes de falha,
  precisão de vertex e quatro renderizações de fragment com leitura de pixels.
  Os valores coincidem com o cálculo de referência, com e sem otimização shaderc
  e com e sem a flag de criação rápida do pipeline.
- Execução do runner real em PowerShell 7, com o processo nativo e a presença do
  MSYS simulados: saída normal, saída 86 com apenas log e saída 86 com shaders.
  Conferidos restauração de pasta/ambiente, desativação de otimização herdada,
  conteúdo do ZIP, arquivos duplicados, antigos e SPIR-V ainda não produzido.
  O arquivo C de shaders também compila com o SDK real para Windows x64.
- Os tempos do compilador acima são observações de uma execução, não limites
  impostos pelos testes. O pipeline sintético também funciona no Lavapipe com
  ambos os modos; isso não prova redução do tempo de criação no driver AMD.

- Teste da espera real: preserva o trabalho que termina em 40 ms; um relógio
  controlado simula builds presos e confirma saída 86 em programa/pipeline/compute.
- Três variantes com desvio (vertex/fragment/compute) compiladas e validadas,
  conferindo `OpLoopMerge` com `DontUnroll` no binário.
- Oito execuções Vulkan com desvio: RGBA8 e R32_UINT, com e sem otimização SPIR-V
  e com e sem a flag de criação rápida. Os valores e os pixels vizinhos coincidem.
- Seis execuções numéricas de surface e regressões de storage/gather preservadas.
- Testes da política Win32 e da sequência de instalação do Job Object com mocks,
  incluindo falha na configuração/associação. Compilação das APIs reais e do
  arquivo de plataforma para Windows x64 com os headers do SDK do Zig.
- Compilação dos arquivos C alterados do backend no Linux.

Os testes Vulkan usam Lavapipe; não são medições do driver AMD proprietário.
O Job Object não foi executado neste ambiente Linux. Não há arquivos do jogo
para medir o boot completo ou verificar a seleção de personagem e os textos.
O novo tempo de abertura e a resposta do Windows continuam dependendo do teste
no computador do usuário; não há promessa de voltar exatamente a cinco segundos.
