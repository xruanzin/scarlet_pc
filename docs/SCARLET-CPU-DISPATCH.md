# Scarlet: despacho de CPU e validação de desempenho

## Diagnóstico desta rodada

Base examinada: `e47141c`, branch `scarlet-bootstrap`.
Os arquivos `fnprof.txt`, `inspect_hot.txt` e o log de boot enviado em
20/10/2026 mostram muitas chamadas a GetCurrentThread (512.898),
SignalLightEvent (471.630) e GetTlsValue (336.267). São contagens de entradas,
não amostras de tempo de CPU. Chamadas ao trampoline e ao SDK podem representar
a mesma operação; não devem ser somadas como trabalho independente.

O log contém períodos de aproximadamente 8–12 FPS com compilação de shaders
zerada. Os tempos do backend são acumulados no intervalo do relatório, não
por frame; incluem esperas. Não provam que todo o tempo restante seja CPU ativa.
As otimizações de segmentos NVN e de despertar seletivo já existem nesta base.

## Alteração

O cache de despacho compartilhado publicava endereço e ponteiro de função em
dois atomics independentes. Uma colisão entre threads podia combinar o endereço
de uma entrada com a função de outra. Release/acquire não torna esse par uma
transação. O teste sintético reproduziu a chamada incorreta no código anterior.

O cache frontal passa a ter 1.024 entradas privadas por thread (16 KiB), sem
escritas concorrentes. A tabela exata de funções continua compartilhada e
imutável durante a execução. A geração invalida caches de threads sobreviventes
a cada boot e após registro de overrides, feito com workers parados. HLE,
pontos de saída para reboot e retomadas de funções permanecem no caminho original.
O cache menor pode aumentar misses em conjuntos de trabalho grandes: o ganho
líquido no jogo deve ser medido, não presumido.

## Testes

- `python tools/test_dispatch_cache.py`: 16 milhões de chamadas em oito threads
  com colisões forçadas; invalidação no reboot, override após cache aquecido e HLE.
- `python tools/test_indirect_resume.py`: 15 testes, incluindo comparação com
  execução ARM64 sintética.
- `python tools/test_guest_setjmp.py`: 2 testes de retorno não local.

Todos passaram em Linux com Zig. O teste de colisões falhou por função errada
quando compilado com o dispatcher anterior. Os harnesses agora fornecem o relógio
exigido pelo profiler e mantêm asserts habilitados mesmo em builds otimizados.
Não foi executado o jogo, nem feito o build completo de Windows. Os tempos do
microteste não representam ganho de FPS e não são um critério de aprovação.

## Comparação no Windows

Compile com `tools/scarlet_build_windows.ps1 -Jobs 4` e rode com
`tools/scarlet_run.ps1`. Compare o mesmo save, câmera e percurso, após aquecer
os shaders, com as mesmas opções nos dois builds. Para medir FPS normal, remova
`LARECOMP_FNPROF`, `LARECOMP_FNPROF_BT` e `LARECOMP_HLE_PROF` do ambiente e não
ative o modo de diagnóstico durante a comparação. Faça três passagens por build.
Envie os logs de boot dos dois builds; registre também qualquer crash ou regressão.
A meta de 30 FPS ainda não foi validada.
