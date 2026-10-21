# Histórico do projeto: o que deu errado e como foi resolvido

Este documento conta, em ordem, como o port do **Pokémon Let's Go Eevee** saiu do zero até o jogo ser jogável
(começar um jogo novo, capturar o Pikachu, salvar e carregar). Cada problema aparece com o **sintoma** (o que se via),
o **diagnóstico** (como a causa foi encontrada), a **causa** e a **correção**, com os arquivos alterados.

Período: 14 e 15/10/2026. Estado atual e pendências: [PROGRESSO.md](PROGRESSO.md). Ferramentas de diagnóstico:
[DEPURACAO.md](DEPURACAO.md).

---

## Parte 1: escolha do caminho

### A ideia
Rodar o jogo do Switch **nativamente no PC**, sem emulador, usando **recompilação estática**: o código ARM64 do jogo
é traduzido para C uma única vez, compilado como um programa Windows, e um *runtime* substitui o sistema do Switch
(kernel, serviços, GPU, áudio, controles).

### Por que o LASRecomp como base
- O [LASRecomp](https://github.com/peiikai/LASRecomp) (Link's Awakening, Switch, licença MIT) já tinha provado que
  o caminho funciona no Switch: tradutor ARM64 → C, kernel/serviços do Horizon e a API gráfica **NVN** traduzida
  para Vulkan.
- A análise ([ANALISE-LASRecomp.md](ANALISE-LASRecomp.md)) mostrou que o núcleo é genérico e que o específico do
  Zelda fica em camadas separáveis (patches por endereço, randomizer, interface).

### Alternativas consideradas
| Alternativa | Decisão | Motivo |
|---|---|---|
| Começar um recompilador do zero | não | o LASRecomp já resolve a parte genérica |
| HWDER (Hyrule Warriors) | plano B para gráficos | recompila o driver NVN e emula comandos Maxwell; licença não declarada, só estudo |
| ssbu-recomp (Smash) | só estudo | GPL-3, incompatível com copiar código para um projeto MIT |
| mk8-recomp | descartado | depende do suyu em execução |
| Reestruturar com CMake (sugestão externa) | não | o Makefile do LASRecomp já funciona; reorganizar não aproxima o objetivo |

### Extração do jogo (regras)
- O jogo vem do **cartucho do próprio usuário** (XCI). Extração com `hactool` usando o `prod.keys` **gerado no
  console do usuário** (Lockpick_RCM). Chaves baixadas de sites foram recusadas: a verificação por hash mostrou que
  as primeiras chaves oferecidas eram idênticas ao pacote do ProdKeys.NET; o dump verdadeiro tinha hash diferente e
  chaves exclusivas do console (`device_key_4x`, `sd_seed`, ...).
- Resultado em `extracted/` (fora do git): `exefs/` (código), `romfs/` (assets) e `romfs.bin` (imagem RomFS crua,
  gerada pelo `hactool --romfs`).
- **Nada do jogo entra no repositório:** arquivos do jogo, chaves, código C gerado, logs, saves e caches estão no
  `.gitignore`. O harness de shader (`tools/skintest`) gera o código derivado do jogo só localmente.

---

## Parte 2: diagnóstico do binário

Detalhes em [DIAGNOSTICO.md](DIAGNOSTICO.md). Em resumo:

- 4 módulos: `main` (o jogo), `sdk` (bibliotecas da Nintendo), `subsdk0` (**glslc**, compilador de shaders) e
  `subsdk1` (**multimedia**, vídeo). O Zelda só tem `main` + `sdk`.
- O tradutor do LASRecomp converteu **100% das instruções** dos 4 módulos (0 não suportadas).
- Title ID confirmado no `main.npdm`: `0100187003A36000`.

---

## Parte 3: montar o fork

1. **Ambiente:** MSYS2 UCRT64 (clang, SDL3, FFmpeg, Vulkan, shaderc) instalado via winget/pacman; Python com
   capstone/unicorn/lz4 em `.venv`.
2. **Remoção da camada do Zelda:** 16 arquivos de HLE de gameplay/randomizer e 11 telas de interface removidos.
   O que o resto do runtime ainda chamava virou stub neutro em `runtime/hle/zelda_stubs.c`.
3. **4 módulos em vez de 2:** loader, tabela de despacho, biblioteca do jogo (`lasgame.dll`, ABI 2) e Makefile
   passaram a tratar `main`, `sdk`, `subsdk0` e `subsdk1` (`runtime/loader.c`, `dispatch.c`, `gamelib.*`,
   `tools/gamelib_entry.c`, `gamebuild.c`, `rt.h`).
4. **Gerador sem os patches do Zelda:** `tools/patches.py` esvaziado (os patches eram endereços do executável do Zelda).

Primeiro build: runtime em segundos; código do jogo (~550 MB de C) em ~22 minutos.

---

## Parte 4: do boot até a tela de título

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 1 | Crash nos construtores do `main` (acesso a `0x1c0`) | disassembly: `nn::os::SetDyingMessageRegion` grava em `[GetInfo(20)] + 0x1c0` | `svcGetInfo` id 20 (contexto de exceção do usuário) devolvia 0 | aloca uma área e devolve o endereço | `runtime/kernel.c` |
| 2 | — (encontrado no caminho) | código do `svcGetInfo` | Title ID do Zelda fixo no kernel | lido do ACI0 do `main.npdm` | `runtime/main.c`, `kernel.c` |
| 3 | Crash em `nn::vi::GetService` | addr2sym no backtrace | `nn::vi::SetLayerScalingMode` sem HLE (o Zelda não usa) | HLE da função | `runtime/hle/vi.c` |
| 4 | Jogo aborta (`svcBreak`) | backtrace nomeado: `nn::btm::user::InitializeBtmUserInterface` | serviços `btm:u`/`bt` eram stubs genéricos sem objetos/eventos | serviços "offline" (Bluetooth sem dispositivos) + `csrng` com números aleatórios | `runtime/services/offline.c` |
| 5 | Jogo parado, nenhum frame | novo dump de threads (`LARECOMP_THREAD_DUMP`) | deadlock de mutex: no timeout de condvar o kernel readquiria o mutex, e o SDK (`TimedWait` → `Enter` em `0xea01`) também | kernel não readquire no timeout | `runtime/kernel.c` |
| 6 | Crash em `nvnCommandBufferBindProgram` | símbolo do runtime via `nm` | o jogo faz bind de programa NULL no início | (depois substituído pelo item 22) | `runtime/nvn/cmdbuf.c` |
| 7 | "Press a button on the controller you want to use" sem aceitar o controle | captura de frame (`LARECOMP_SHOT_AT`) + `SetSupportedNpadStyleSet 0x5a` | o runtime apresentava um **Pro Controller**, que o Let's Go não aceita | controle como **modo portátil** (configurável por `LARECOMP_NPAD`) | `runtime/hle/input.c`, `services/hid_set.c` |
| 8 | Crash ao abrir o vídeo de título | backtrace: `movie::MediaData::FindInt64` | 2 das 28 funções de vídeo sem HLE (já previsto no diagnóstico) | `FindInt64` e `SetCoreMask` | `runtime/hle/movie.c` |
| 9 | ~240 fps, áudio picotado | estatística do renderer | o runtime ignorava o *present interval* do jogo | espera pelo intervalo do jogo (2 = **30 fps**), `LARECOMP_FPS` para mudar | `runtime/nvn/objects.c` |

### O vídeo de título (o caminho mais longo)
| # | Sintoma | Diagnóstico | Causa | Correção |
|---|---|---|---|---|
| 10 | GPU "device lost" (escrita no endereço 0) | camada de validação do Vulkan | texturas do vídeo eram encolhidas pela **escala de resolução interna** e a cópia em tamanho cheio estourava | texturas preenchidas pela CPU ficam em tamanho cheio (`vk_tex_unscale`) |
| 11 | Tela verde (NV12 com Y=UV=0) | testes por eliminação: sem escala, buffers no heap do jogo, preencher com cinza → continuava verde | — (hipóteses descartadas, mas viraram melhorias: `backend_pool_written`, alocador do jogo) | ver item 12 |
| 12 | Tela verde | log de quem lê os quadros: o jogo nunca chamava `Buffer::Base` no vídeo; `GetOutputBuffer` recebia um **buffer do próprio jogo** (1920×1080×1,5) | o runtime trocava o buffer do jogo pelo seu; o jogo continuava lendo o seu, vazio | o quadro é copiado no buffer do jogo, em NV12 compacto |
| 13 | Áudio do vídeo "robótico" | gravação WAV da saída + comparação com o arquivo original (70% das amostras exatas) + teste de ouvido do usuário | o decoder reusava na hora o buffer de saída recém-liberado, enquanto o jogo ainda copiava dele | slots de saída em rodízio |
| 14 | Música e efeitos mudos (saída de 6 canais toda em zero) | gravação WAV por saída | `hwopus` (decodificador Opus) era stub: o jogo desistia | serviço `hwopus` implementado com o Opus do FFmpeg |
| 15 | Vídeo congelava em quadro aleatório | dump de threads: thread de vídeo esperando um mutex **dela mesma** | corrida no kernel: um sinal chegava junto com o timeout, o kernel entregava o mutex e mesmo assim respondia "timeout" | mantém o resultado de quem acordou a thread |
| 16 | O jogo nunca saía do vídeo (tela branca no fim) | estado dos decoders + contagem de pacotes do arquivo (3.458) | no fim do arquivo o extrator devolvia `1` (erro genérico, "tente de novo") | devolve `0x2000`, o "fim de stream" de verdade, achado na própria biblioteca do jogo (`NnMediaCommon::AndroidErrorToNnError`) |

Arquivos: `runtime/hle/movie.c`, `runtime/nvn/vulkan/textures.c`, `exec.c`, `runtime/services/hwopus.c`,
`runtime/services/audio.c`, `runtime/kernel.c`.

---

## Parte 5: introdução, 3D e gameplay

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 17 | Nome "RRS" aparecia como "?S" | bytes: `52 52 53` lidos como UTF-16 = `U+5252` + `S` | o teclado devolvia UTF-8; o Let's Go lê UTF-16 | resposta em UTF-16LE | `runtime/hle/swkbd.c` |
| 18 | Sem 3D (tela preta com som no quarto) | contagem de draws pulados por motivo: todos "noobj" | o jogo faz bind de **programas separados por estágio** (vertex num, fragment noutro, NULL para desligar); o runtime trocava o programa inteiro a cada bind | programa combinado por estágio (`bind_program_stages`) | `runtime/nvn/cmdbuf.c`, `vulkan/exec.c` |
| 19 | ~4.000 draws/2 s ainda pulados | log dos estágios: `0x1` | programas só com vertex shader (passes de profundidade/sombra) | fragment shader vazio sintetizado | `runtime/nvn/vulkan/exec.c` |
| 20 | Mira da Poké Ball ia para o céu | — (relato do usuário) | sensor de movimento (`SixAxisSensor`) sem implementação: o SDK lia lixo | sensor neutro (parado, nivelado, apontado para a frente) | `runtime/hle/input.c` |

Resultado: **cenário 3D correto** (quarto, Pallet Town, céu), captura do Pikachu, **save e load funcionando**
(`save/save_0/savedata.bin`).

---

## Parte 6: personagens "explodidos" (resolvido)

Os modelos com esqueleto (personagens, Pokémon) apareciam esticados em "espinhos" que convergiam para um ponto.
O cenário estático estava correto, então câmera e geometria comum funcionavam. Plano e experimentos completos em
[PLANO-3D.md](PLANO-3D.md).

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 21 | Personagens e Pokémon em espinhos | captura no RenderDoc + o shader rodado na CPU com os mesmos dados da GPU: saídas idênticas, e o `w` de saída proporcional ao peso do **1º** osso | `LDC.64 R4, c[4][R4+0x38]` (carrega R4 e R5 com R4 como endereço) virava `r4 = cb4[r4+56]; r5 = cb4[r4+60]`: a segunda leitura usava o R4 **já sobrescrito**. O `w` da translação do 2º osso (que deveria ser 1,0) vinha zero ou lixo, e o vértice ia para perto do plano da câmera | o tradutor copia o registrador de endereço para uma temporária quando ele também é destino | `runtime/nvn/maxwell/translate.c` |

### O caminho até a causa
1. **Descartados com dados** (antes do plano): formato de atributo, índices e pesos em memória, matrizes da paleta,
   momento da cópia dos uniforms, cópia dos vértices.
2. **Fases 0 e 1** do plano: reprodução determinística com o save (`tools/repro.sh`) e cortes binários na GPU. Peso
   100% no 1º osso deixava os modelos inteiros; o 2º osso estava errado. A conclusão da época, "o valor de `a11.y`
   chega errado na GPU", estava **errada**: o teste do shader na CPU (X5) tinha passado por coincidência, porque o
   vértice usado não exercitava a leitura defeituosa.
3. **Fase 3, RenderDoc**: os bytes de vértice que a GPU lê (incluindo `a11 = [7, 6]` e os pesos) eram idênticos aos
   da memória. Rodar o shader traduzido na CPU com os dados **exatos** da GPU (vértices e `cb3`/`cb4` exportados da
   captura) reproduziu a saída da GPU número por número. Então o defeito estava no próprio shader traduzido. O log
   das leituras de `cb4` no harness e a leitura do GLSL apontaram as duas linhas acima.

Enquanto a correção não existia, o `jogar.bat` usou um paliativo (`LARECOMP_VK_SKIN_ONEBONE=1`, peso 100% no 1º
osso: modelos inteiros, animação mais rígida).

---

## Parte 7: interface

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 22 | Ícones dos Pokémon "picotados" (equipe, tela de experiência após captura, Pokédex) | log das texturas do draw (`68x56`, formato `0x3B`) e decodificação da textura gravada com 4 layouts de bits: só "R nos bits baixos, alfa no bit 15" dá o ícone certo | o NVN `RGB5A1` (e o `RGBA4`) tem o vermelho nos bits baixos; o Vulkan `R5G5B5A1_UNORM_PACK16` tem o vermelho nos altos e o alfa no bit 0: cores trocadas e alfa tirado de um bit de cor | convertidos para RGBA8 no upload, como o RGB8 | `runtime/nvn/vulkan/textures.c` |
| 23 | Flash azul e depois preto ao abrir o jogo | prints dos primeiros frames e log das limpezas (`LARECOMP_LOG_CLEARS`, agora com a cor): os 2 primeiros presents são um quad 2D azul de tela cheia desenhado pelo próprio jogo, sem nenhum clear; o `nn::oe::FinishStartupLogo` é chamado já no frame 0, então o logo do sistema não os esconderia | não esclarecida (não se sabe se o console mostra o mesmo) | a janela fica preta nos 2 primeiros presents (`LARECOMP_HIDE_FIRST_FRAMES`, 0 desliga) | `runtime/nvn/vulkan/device.c` |
| 24 | Alguns objetos nunca apareciam (1247 draws pulados como `failed` em 4 min de jogo) | `tools/perf_summary.py` no log de uma sessão longa; o log tinha `FS translate failed: unknown instruction df5800812ff70400` | o tradutor de shaders não conhecia a `TMML` (consulta do nível de mipmap que uma amostra usaria): o shader inteiro falhava e todo draw com ele era pulado. O shader do jogo usa o nível para limitar a amostra (`min(lod, 2)`) | `TMML` traduzida com `textureQueryLod` (ponto fixo 8.8, componentes na ordem inversa do GLSL, como no Ryujinx) | `runtime/nvn/maxwell/translate.c` |
| 25 | Objetos aparecendo de novo na primeira vez em cada área depois de um crash | data do `cache/vk_pipelines.bin` | o cache de pipelines só era gravado ao fechar o jogo normalmente | gravado numa thread de fundo ~6 s depois que uma leva de pipelines novos termina (~90 ms, sem travar quadro) | `runtime/nvn/vulkan/exec.c` |


## Parte 8: qualidade de imagem e pop-in (15/10/2026)

Auditoria de um quadro da Rota 1 (`LARECOMP_DEBUG_FRAME`, imagens de cada passe) e da camada de validação do Vulkan:
o quadro é mapa de sombras 2048² → cena HDR 1080p → cadeia de bloom (960x540 até 10x6) → composição → interface, sem
draw faltando. A validação acusa só uma textura RGBA8 16x16 (o "sem sombra" do próprio jogo) lida por um amostrador
de sombra: fora da especificação, mas a GPU NVIDIA compara o canal vermelho como o Tegra do Switch.

| # | O quê | Medição | Mudança | Arquivo |
|---|---|---|---|---|
| 26 | Serrilhado: o jogo não tem antisserrilhado | prints da janela com e sem | o SMAA 1x herdado do LASRecomp (opção do menu Esc do Zelda, removido) volta ligado por padrão, com a caixa "Suavizar serrilhado" no launcher (chave nova `smaa` no ini: o `antialias = off` antigo era só o padrão salvo) | `runtime/settings.c`, `runtime/ui/launcher_lg.mm` |
| 27 | Supersampling (1440p / 4K numa tela menor) reduzido com um só tap bilinear, que pula texels | sem limite de fps, gameplay a ~160 fps em 1080p, ~115 em 1440p e ~78 em 4K (RTX 3080): 4K numa tela 1080p sobra para 30 fps | a apresentação usa 4 taps bilineares espalhados na área de cada pixel da janela (em 2x exato, a média 2x2) | `runtime/nvn/vulkan/shaders.c` |
| 28 | Texturas borradas em ângulo rasante (caminhos, grama ao fundo) | o jogo pede pouca ou nenhuma filtragem anisotrópica | 16x nos amostradores com mipmap e filtro linear (`LARECOMP_ANISO`) | `runtime/nvn/vulkan/textures.c` |
| 29 | Objetos surgindo alguns quadros depois de cada leva de shaders novos | ~20-60 draws pulados por leva na gameplay; com cache, ~1 ms por programa | o draw espera o programa / pipeline ficar pronto, até 20 ms por quadro (a 30 fps sobram ~27 ms), em vez de ser pulado: zero draws pulados na gameplay, fps igual; os que sobram ficam na tela de carregamento (`LARECOMP_SHADER_WAIT_MS`) | `runtime/nvn/vulkan/exec.c` |
| 30 | Primeira visita a uma área: shaders prontos devagar | cache vazio (pasta `cache/` afastada): tradução ~240 ms numa thread só, enquanto as 16 threads de compilação esperavam; depois disso, o que pesa é o driver criando pipelines (~30 ms cada) | a tradução roda nas threads de trabalho (o buffer de strings `RSC2` do tradutor passou a ser por thread) e compila em seguida na mesma thread. Mesmo com cache vazio, toda a preparação cai nos quadros pretos do carregamento: a área aparece completa (prints da transição) | `runtime/nvn/maxwell/translate.c`, `runtime/nvn/vulkan/exec.c` |
| 31 | Mouse sem função; teclado com o layout do Zelda (K/J/L/I) | encontro achado com tentativas (`LARECOMP_GYRO_TEST`, o jogo só liga o sensor depois de "Get ready") | teclado com layout do Let's Go; mouse preso à janela com um clique: botões = A / B, movimento = giroscópio virtual (depois, #34: preso sozinho só durante a mira; fora dela é o dedo na tela de toque) | `runtime/hle/bindings.c`, `runtime/hle/input.c` |
| 32 | Mira da Poké Ball parada no modo portátil | o jogo leu o sensor só 2 vezes por captura: `IsSixAxisSensorAtRest` respondia sempre "parado" e o jogo pula a leitura de um sensor parado | nunca "parado" (um console na mão nunca está); orientação, ângulo, velocidade angular e gravidade coerentes; eixo z = mira horizontal, x = vertical. O jogo mede o giro a partir do início da mira e ignora saltos bruscos (ruído): só movimento contínuo, como o do mouse, mexe a câmera | `runtime/hle/input.c` |
| 33 | Sem como jogar traduzido (o jogo não tem português) | o runtime já tinha um LayeredFS completo (trocar e adicionar arquivos do romfs) usado pelas seeds do randomizer do Zelda, desligado no Let's Go; a tradução PT-BR de fãs vem como pasta do Atmosphere e troca os textos de `English` e `Spanish`, a interface (`bin/app`) e imagens | `runtime/mods.c`: a pasta `mods/<nome>/` escolhida no launcher alimenta o mesmo mecanismo (sem desviar os saves, como a seed fazia). O idioma pedido ao sistema virou configurável (`game_lang`), mas o jogo segue o idioma gravado no save: trocar só ele não muda os textos de um save existente | `runtime/mods.c`, `runtime/services/fs.c`, `runtime/ui/launcher_lg.mm` |
| 34 | "Brincar com Eevee" não respondia | o modo portátil acaricia pela tela de toque; o jogo importa `GetTouchScreenStates<16>` e `GetGestureStates`, nenhum dos dois implementado (lia memória vazia) | `GetTouchScreenStates` devolve um dedo: o mouse livre sobre a imagem do jogo (convertido para a tela de 1280x720 do Switch; a escala certa é a da TV, 1920x1080: #36), com Start / End no começo e no fim do toque; testado acariciando a testa: o Eevee fecha os olhos, notas musicais e brilhos. Gestos: o jogo consulta, mas o carinho não depende deles (devolve nenhum) | `runtime/hle/input.c`, `runtime/nvn/vulkan/device.c` |
| 35 | Mouse e teclado reais interferindo nos testes automáticos | posição do mouse do usuário aparecendo no log de um teste | o filtro `input_user_blocked` só existia no backend Metal; o laço de eventos do Vulkan não o chamava | `runtime/nvn/vulkan/device.c` |
| 36 | Botão de frutas do "Brincar com Eevee" não abria | toques no ícone com vários formatos (rápido, segurado, arrastado) e dia adiantado; o disassembly do leitor de toques do jogo (`0x70007c1ac4`) mostrou que ele confere a posição de cada amostra, inclusive a de soltar; com a coordenada em escala 1920x1080 o botão abriu | o runtime diz ao jogo que está na TV (1920x1080) e o jogo usa o toque nessa escala, não nos 1280x720 do portátil: o ícone ficava a 2/3 do lugar. Toques agora em pixels da tela informada (`LARECOMP_DISPLAY_W`), e a amostra de soltar repete a última posição (antes ia para a posição padrão / do mouse) | `runtime/hle/input.c` |
| 37 | Brincar com Eevee impossível só com controle | o modo portátil depende da tela de toque | dedo virtual: analógico direito move, ZR segurado toca (ZR não chega ao jogo, onde só repete o A), círculo desenhado pelo overlay (`runtime/ui/menu.mm`). Testado o fluxo inteiro: abrir as frutas, fechar o tutorial com A, pegar a Fruta de Razz, levar à boca: ×23 → ×22 | `runtime/hle/input.c`, `runtime/ui/menu.mm` |
| 38 | Restos do Link's Awakening no runtime | levantamento de cada função neutralizada em `hle/zelda_stubs.c` e de quem a chamava | o fork tinha desligado a camada do Zelda com stubs, mas as chamadas e as telas continuavam no código | removidos: os stubs e as chamadas (warp, pets, portas, potes dourados, garrafa, corações, tela larga das masmorras, editor de save, troca de nível por nome), o randomizer, o troca-ícones de botão por textura do Zelda (`texpatch.c`), as abas antigas do menu Esc (o menu agora só tem a pergunta de fechar, a digitação de nomes, o fps e o dedo do controle), o launcher antigo (de Mac), a montagem do código do jogo na máquina do jogador (`gamebuild.c`, zig) e o backend Metal de Mac, que nunca foi compilado neste projeto. As opções do Zelda saíram do `larecomp.ini` (as antigas são ignoradas) | `runtime/` |
| 39 | Jogar exigia MSYS2, `make gen`, `make` e um `jogar.bat` | comparação com outros ports nativos (Zelda64Recomp, UnleashedRecomp, o próprio LAS Recomp): um zip com o exe, a pasta do jogo escolhida na primeira tela, nada para instalar | o fluxo de release do LAS Recomp (gerador congelado, Zig, `gamelib-src/`) tinha sido removido junto com o resto do Zelda | `runtime/gamesetup.c` (a pasta escolhida no launcher: procura `exefs` e `romfs` / `romfs.bin`, confere o build id da v1.0.0, copia para `extracted/` e recompila o código do jogo com `runtime/gamebuild.c`, os 4 módulos); launcher em inglês ou português (idioma do Windows), com progresso e erros explicados; "Instalar tradução..." (`runtime/mods.c`: abre .zip/.rar/.7z com o `tar` do Windows e acha o romfs do Eevee); `make dist` (FFmpeg só LGPL, gerador congelado, Zig enxuto) → `LetsGoRecomp.exe`. Armadilha nos testes: o `timeout` do MSYS2 não repassa variáveis de ambiente a exes nativos |
| 40 | Na release 0.1.0, o jogo fechava logo depois da tela do controle (`svcBreak` 2111-0001) | teste da release baixada do GitHub numa pasta nova, sem MSYS2: o log mostrava `[hwopus] FFmpeg has no Opus decoder` logo antes; `config_components.h` do FFmpeg da release: `CONFIG_OPUS_DECODER 0` | o script de dependências herdado do LAS Recomp (`--disable-swresample`) desligava em silêncio o decodificador Opus, que depende do swresample; o Zelda não usa Opus, o Let's Go usa em toda a música e efeitos | swresample ligado e o script falha se faltar qualquer decodificador que o jogo usa (`tools/build_deps_windows.sh`); sem Opus o runtime para com uma mensagem clara (`runtime/services/hwopus.c`). Release 0.1.1. Lição: testar a release como um jogador, até passar da tela do controle, antes de publicar |
| 41 | Barra da montagem do jogo em 100% minutos antes do fim | captura da janela do launcher durante a compilação | o progresso do gerador somava 0,5 por módulo: fechava em 1 com os 2 módulos do Zelda, mas chegava a 2 com os 4 do Let's Go | cada módulo vale 1/4 (`runtime/gamebuild.c`) |
| 42 | Na release 0.1.1, jogo novo (sem save) fechava na tela de título (`exception c0000005 accessing 0x39`) | teste do visitante em duas fases (montagem pelo launcher; depois jogo novo com entrada roteirizada): `[movie] cannot open extracted/romfs/bin/movies/EEVEE_GO/title_male.webm` logo antes | o player de vídeo (HLE) abria os vídeos direto da pasta `extracted/romfs/`, que só existe no ambiente de desenvolvimento; a montagem da release cria só a imagem `romfs.bin`. Com save, o vídeo do título não toca, por isso o teste anterior passou | os vídeos são lidos de dentro do `romfs.bin` por um leitor próprio da FFmpeg sobre o índice do `fs.c` (`romfs_image_file`); ordem: arquivo de um mod, pasta `romfs/`, imagem (`runtime/hle/movie.c`, `runtime/services/fs.c`). Release 0.1.2 |
| 43 | A documentação afirmava que o jogo não tem passo de tempo global, e o 60 fps no mundo funcionava sem explicação | levantamento de todas as leituras da tabela `G` no `main` (`adrp`+`ldr`, 9 instruções), sonda do passo recebido (`LARECOMP_ELAPSED_LOG`) e contagem de quem lê cada passo (`LARECOMP_TIMER_USERS`) | a varredura antiga procurava floats perto de 1/30; o passo é um inteiro em ns num objeto de temporizadores na pilha da thread principal: o passo 1 vem de `G[1]` pelo accessor `f_7000039600` a cada atualização do mundo, o passo 0 é fixo. Só os sistemas que convertem tempo em quadros pelo passo 1 seguem o relógio; contadores de quadro (captura, batalha) não | PROGRESSO e PESQUISA-60FPS corrigidos ("Mapa temporal"); o modo 2 (contadores divididos) mostrou-se insuficiente para a batalha (`tools/teste60.sh`): batalha e captura ficam com lógica a 30 |
| 44 | 60 fps (modo 1): na captura e na batalha o quadro intermediário tremia, pulava e misturava a interface (títulos sobrepostos, painéis do menu fora do lugar, retângulos brancos) | pares intermediário/final gravados no mesmo tempo de cena (`LARECOMP_WINDOW_SHOTS_REL`), diagnóstico por buffer e motivo de rejeição (`LARECOMP_INTERP_DIAG`), métrica de pixels inventados (`tools/interp_metrics.py`) | três causas: (1) os ganchos que marcam o objeto de cada desenho só eram instalados se a interpolação estivesse ligada no boot, e no modo 1 ela só liga dentro das cenas: tudo era pareado pela ordem (fantasmas, interface misturada); (2) os desenhos de interface também caem dentro de objetos, então "tem objeto" não separava 3D de 2D; (3) a mistura era palavra a palavra, e uma linha de matriz com um componente misturado e outro mantido (cruzando o zero, ou um inteiro no lugar de um float) deforma o modelo; além disso o intervalo até a imagem final herdava os 16,7 ms do mundo no começo de cada cena | ganchos instalados sempre que o modo 60 está ligado (`cmdbuf.c`); só se misturam desenhos com objeto, alvo com profundidade e teste de profundidade ligado (interface fica com o quadro atual); mistura por vec4, tudo ou nada, aceitando 0 como float; período fixo de 33,3 ms no modo 60. Medido: batalha 42 de 47 quadros sem pixels inventados (os 5 restantes são cortes e movimentos grandes de câmera, coerentes na imagem), captura 0 de 46 |
| 45 | 60 fps (modo 1): na batalha, personagens esticados e Pokémon deformados no quadro intermediário (a captura já estava boa) | batalhas roteirizadas com pares intermediário/final a cada 4 quadros nos ataques (`tools/teste60.sh batalha 1`), métrica de pixels inventados e imagens ampliadas dos quadros apontados | (1) corte de câmera (Eevee → close da treinadora): os ossos da treinadora longe e de perto eram misturados (o limiar de "salto" aceitava mudanças de até ~100%); (2) a mistura por vec4 deixava parte de uma paleta de ossos em N e parte no meio; (3) sombras: o mapa de sombra era redesenhado com as matrizes misturadas, mas o chão (sem objeto do jogo) usava a matriz de luz de N e a sombra escorregava | antes do redesenho, cada objeto mede quanto seus buffers mudaram: acima de 20% das palavras com passo grande ele fica com a pose de N; se os rejeitados concentram a maior parte da mudança (corte), não há intermediário; um salto em qualquer palavra deixa o buffer inteiro em N; todo desenho 3D (alvo com profundidade e teste ligado) é misturado, com ou sem objeto (os sem objeto formam um grupo). Medido em duas batalhas: 1 de 201 quadros apontado, coerente na imagem (zoom rápido de câmera) |
| 46 | Mesmo com as correções 44 e 45, a interpolação nas batalhas e capturas seguiu ruim no teste em jogo | avaliação do jogador em ritmo real | as medições automáticas (pixels inventados) não captavam o que incomoda em movimento | interpolação retirada do modo 60 (`exec.c`: só com `LARECOMP_INTERP=1`, pesquisa); batalhas e capturas ficam a 30 sem quadro intermediário; mundo, menus e Pokédex seguem a 60 nativos |
| 47 | 60 fps (modo 1): menus e Pokédex acelerados | menu aberto com `X` a 30 e no modo 1, cada quadro gravado (`LARECOMP_SCREENSHOT_EVERY=1`): a bola do menu aparecia em 4 quadros de jogo em vez de 8 | desde o commit "menus a 60", todo quadro fora de batalha/captura ia a 60; o menu para o mundo e conta os próprios quadros | só vai a 60 o quadro em que a atualização do mundo (`f_70002f8bb0`) rodou; menus, Pokédex e outras telas que param o mundo ficam a 30 (medido: idêntico ao de 30 quadro a quadro) |
| 48 | Depois da 47, menus e Pokédex ficaram sem aceleração, mas travados a 30 (o jogador lembrava de uma versão em que estavam perfeitos a 60) | comparação com as versões testadas: a "perfeita" era a com quadro intermediário nessas telas | menus e Pokédex só ficam fluidos e na velocidade certa com a lógica a 30 e o quadro intermediário; a 60 nativo eles aceleram | interpolação ligada só em menus (`fps60_menu`: mundo parado, sem batalha ou captura), período fixo de 33,3 ms; batalha e captura seguem sem ela. Medido no menu aberto com `X`: intermediários sem pixels inventados |
| 49 | Pokédex travada a 30 mesmo com o quadro intermediário nos menus; na abertura o contador oscilava entre 30 e 60 | rolagem da lista com cada par intermediário/final gravado; diagnóstico de rejeição (`LARECOMP_INTERP_DIAG`) | (1) a Pokédex é 2D, e o 2D ficava fora da mistura; (2) o detector de corte da batalha rejeitava todo quadro de menu (quase toda mudança da tela é grande); (3) com o 2D ligado, linhas recicladas da lista cruzavam a tela no meio; (4) na abertura o intermediário ligava e desligava (telas sem mundo, carregamentos) | nos menus o 2D também é misturado, sem o detector de corte, com teste de salto estrito para 2D (mudança acima de 30%: o desenho fica em N); intermediário de menu só depois de o mundo rodar a primeira vez. Medido: rolagem da Pokédex 25 de 25 quadros interpolados sem pixels inventados; menu principal sem pixels inventados |
| 50 | Na mira da captura, com o analógico solto, a mira continuava andando; qualquer toque já movia | leitura do analógico em `runtime/hle/input.c` | o valor bruto do SDL ia direto para o jogo, sem zona morta (um controle parado lê algumas centenas, e o jogo trata qualquer valor como empurrão) | zona morta radial com reescala (`stick_deadzone`, padrão 0,15, ajustável no launcher em "Teclado e mouse": "Zona morta do analógico"), também no desvio simulado dos testes |
| 51 | 60 fps: piscada feia ao abrir o menu andando (parado quase não acontecia) | menu aberto duas vezes andando, pares intermediário/final gravados na segunda abertura | com a interpolação desligada (mundo), os dois quadros gravados da última vez que ela esteve ligada continuavam marcados como válidos: o primeiro intermediário do menu seguinte misturava a cena atual com a do menu anterior | os quadros gravados são descartados sempre que a interpolação desliga; medido: segunda abertura andando sem pixels inventados |
| 52 | Issue #1: Let's Go Pikachu 1.0.2 numa AMD com Win10 fechava no boot (`c0000005` em `0x38`, fonte), abortava nas configurações do GO Park e o launcher recusava o Pikachu; o patch enviado fazia o setup aceitar "qualquer versão" | leitura do patch e do log do usuário; busca de endereços fixos no runtime e no gerador; teste com o Pikachu 1.0.0 do próprio usuário (setup pelo launcher, jogo novo roteirizado até a tela do nome) | (1) a Yu Gothic Regular de alguns Win10 quebra o motor de fontes do jogo; (2) o `btm:u` não tinha o `AcquireBlePairingEvent` (cmd 21); (3) o setup conferia o build id do Eevee 1.0.0. Aceitar outra versão do jeito do patch quebraria a 0.2.0: o gerador troca 3 instruções em endereços do Eevee 1.0.0 (`tools/patches.py`, sempre) e os ganchos de 60 fps, batalhas selvagens e objetos são endereços desse `main` | setup aceita os dois jogos pelo program id (`main.npdm`); paridade (regra do projeto): os endereços do Eevee 1.0.0 foram achados no `main` do Pikachu 1.0.0 comparando instrução por instrução, com os campos relativos mascarados (`tools/addrmap.py`: deslocamentos de -0x30 a +0x10, offsets de struct idênticos, a tabela `G` e o slot `0x15fab08` no mesmo lugar); os ganchos continuam escritos com o endereço do Eevee e o `fnhook` traduz pelo build (`rt_fnhook_set_build`, `rt_addr`); o gerador move os 3 remendos (`BUILD_ADDRS`, `tools/patches.py`); outros builds (atualizações) ficam sem eles e o launcher desativa as duas opções; versão real informada ao jogo (`game_version`); espera ignora slot de handle vazio; dump plano; save do Pikachu em `save-pikachu/`; tradução pelo title id do jogo montado; Yu Gothic Medium e MS Gothic antes da Regular; cmd 21; menu sobreposto em EN/PT/RU; últimas chamadas ao host no relatório de crash e `lasrecomp-prev.log`. Release 0.2.1 |
| 53 | Com 60 fps e batalhas selvagens ligados (0.2.0, nos dois jogos), a batalha selvagem ganhava quadro intermediário e ficava "ridícula" (o jogador viu no Pikachu; o teste da 0.2.0 só conferiu que a batalha começava) | log da sessão do jogador: depois de `encounter intercepted` nenhum `scene start`; o mesmo no log do teste da 0.2.0 com o Eevee | a batalha selvagem é montada pelo processo das batalhas estáticas (`wildbattle.c`), que nunca chama `BATTLE_START`: sem `g_special`, o mundo parado contava como menu (`fps60_menu`) e ia para a interpolação | o modo batalhas selvagens avisa o 60 fps ao montar a batalha (`fps60_scene_begin`); o log do 60 fps separa os três estados (mundo / menu com intermediário / batalha e captura). Conferido por roteiro no Eevee (encostou na Rota 1: `battle / capture: 30 fps`) e jogando nos dois jogos. Lição: testar as opções juntas, não só cada uma |
| 54 | Issue #1 (segunda mensagem, que só vi depois de responder a primeira): troca local, Presente Misterioso e cooperativo pela internet fechavam o jogo | patch v2 do usuário; o `service_lookup` (`runtime/ipc.c`): serviço sem implementação vira um stub que responde ok sem devolver objeto | `nifm:u`, `ssl`, `bsd:u` e `ldn:u` não existiam: `CreateGeneralService` / `CreateUserLocalCommunicationService` voltavam sem sessão e o jogo usava um ponteiro nulo | `runtime/services/network.c`: console offline (nifm sem internet 2110-0300, eventos já sinalizados, socket com ENETDOWN, scan com 0 redes); os demais comandos do `btm:u` sem dados. Testado por roteiro nos dois jogos: troca local procura e o B volta, Presente Misterioso fica em "Connecting..." (o jogo não tem saída offline ali), GO Park procura e o B volta. Lição: ler a issue inteira antes de responder. Release 0.2.2 |

O que sobra numa primeira vez: um objeto nunca visto antes (Pokémon ou efeito novo) no meio da gameplay pode surgir 1-2
quadros depois, porque o driver leva ~30 ms para criar o pipeline e a espera por quadro é de 20 ms. Da segunda vez em
diante, o cache resolve.

Sombras: em 4K as bordas já saem suaves (o jogo filtra as sombras); um mapa maior que 2048² traria pouco e mexeria na
escala de alvos que não são 16:9 - fica de fora. Física e lógica rodam o código do próprio jogo, recompilado: o
comportamento é o do Switch.

---

## Lições aprendidas
- **Medir antes de chutar.** Os maiores avanços vieram de instrumentação: dump de threads, motivo dos draws
  pulados, gravação do áudio, log de quem chama cada função.
- **O LASRecomp é "certo para o Zelda".** Vários bugs eram suposições que valiam para o Zelda e não para o Let's Go:
  formato de texto, Pro Controller, programas por estágio, buffer de vídeo do jogo, código de fim de stream.
- **A biblioteca do próprio jogo é a referência.** Ler o disassembly do SDK e do `subsdk1` (por exemplo
  `TimedWait`, `AndroidErrorToNnError`) deu a resposta exata em vez de tentativa e erro.
- **Corridas de thread aparecem como sintomas aleatórios.** O vídeo travava num quadro diferente a cada execução;
  a causa era uma corrida no kernel, não o vídeo.
- **Um teste que passa precisa exercitar o caso que falha.** O shader na CPU "provou" que a tradução estava certa
  com vértices que não passavam pela instrução defeituosa. Rodar com os dados exatos de um draw quebrado (exportados
  do RenderDoc) e comparar com a saída da GPU resolveu em minutos.
- **Tradução de registradores: cuidado com destino que também é fonte.** Instruções que escrevem vários
  registradores (`LDC.64`, `.128`) podem ter o endereço entre os destinos; leia as fontes antes de escrever.

## Giroscópio real do controle (16/10/2026, não validado em hardware)
Ideia tirada do `hid_motion.c` do ScarletNX (reimplementada, sem copiar código): o controle com sensor (DualSense, Pro
Controller) vira a mira como o mouse. `runtime/hle/input.c` liga `SDL_SENSOR_GYRO` ao abrir o controle e integra cada
`SDL_EVENT_GAMEPAD_SENSOR_UPDATE` (rad/s × tempo entre amostras) em yaw/pitch do giroscópio virtual, só enquanto o jogo
mira; zona morta de 0,015 rad/s contra o viés em repouso, amostras com intervalo > 100 ms descartadas. Opções:
`pad_gyro_sens` (launcher) e `LARECOMP_GYRO_PAD_FLIP`. Compila (`input.o`, `settings.o`); sentido dos eixos por conferir.

## Modo "batalhas selvagens" (17/10/2026, experimental)
Ideia: ao encostar num Pokémon selvagem, em vez da captura estilo Pokémon GO, uma batalha clássica e, depois, a captura,
como o jogo já faz com o Snorlax, as aves lendárias e o Mewtwo. Detalhes e endereços em [MODO-BATALHA.md](MODO-BATALHA.md);
código em `runtime/hle/wildbattle.c`; opção "Batalhas selvagens (experimental)" no launcher (`wild_battle = 1` no
`larecomp.ini`) ou `LARECOMP_WILDBATTLE=1`.
- **Como funciona.** As batalhas "estáticas" são o processo da classe A (fábrica `17f0a0`), que lê o registro de
  `bin/script_event_data/event_encount.bin` pelo hash em `[obj+0x98]`. O gancho no gatilho do toque (`305ad0`, substituído
  inteiro: a função original configura depois o handle da cena de captura, que um processo de batalha não aceita) cria
  esse processo; o gancho no construtor grava um hash nosso; o gancho na busca (`2bad00`) responde com uma cópia do
  registro do Snorlax com espécie, forma, nível, gênero e IVs do Pokémon tocado (lidos do objeto `P` do construtor da
  configuração: nível = byte `P+0xf0c`, IVs = `P+0xf18..0xf2c`, gênero = `P+0x40c`) e os golpes por nível da
  `waza_oboe.gfpak`. Depois da batalha, a captura do mesmo Pokémon é o fluxo nativo.
- **Sintoma: tela preta ao voltar ao mundo.** Diagnóstico: o mundo era desenhado (passes grandes) e só os passes finais
  saíam pretos; `LARECOMP_DEBUG_FRAME`/`g_debug_frame_request` e o log das chamadas de fade (`11ece0`, `11ee50`) mostraram um
  fade out tipo 1 ao fim da captura e **nenhum fade in** até a próxima captura começar sozinha. Causa: o script do evento
  (`FadeOut_` -> `CallWildBattle` -> `GetWildBattleResult_` -> `FadeIn_`) faz o fade in; nós pulamos o script. Correção: o
  gancho no `2f78d0` (o mundo, todo quadro) repete 30 quadros depois da batalha um fade in do tipo do último fade out.
- **Sintoma: batalha começava com flags trocadas.** `CallWildBattle` grava `a0 = a1 = 0, a2 = 1` e o construtor deixa
  `a0 = a1 = 1, a2 = 0`; copiamos os valores do script.
- **Sintoma: queda ao copiar a "configuração real".** Duas tentativas de lançar a configuração (0x2b8 bytes) do Pokémon
  por cima da da batalha derrubaram o jogo: o objeto de `x0` no gatilho tem 0xb8 bytes (não é a configuração) e a que o
  construtor devolve ainda estava com a espécie zerada. Desligado (`LARECOMP_WILDBATTLE_USECFG`, padrão 0); o caminho que
  funciona é ler os campos de `P` e montar o registro.
- **Lições.** (1) Procure primeiro o que a comunidade (pkNX) e o próprio jogo já dizem: os nomes dos nativos de script
  estavam em strings do `main`. (2) Roteirizar o teste (`LARECOMP_INPUT`) e salvar imagens a cada N quadros mostrou em
  minutos o que três rodadas manuais não mostraram. (3) O `LARECOMP_HOOK_DUMP` exige o endereço completo do guest.
- **Falta:** XP dividido (metade ao derrotar, metade ao capturar), natureza e shiny do Pokémon tocado, formas
  regionais (a tabela de golpes usa o índice da espécie), batalha de Pokémon grandes/auras e teste em mais áreas.


## Scarlet — escopos de imagem e espera de interpolação (20/10/2026)

**Sintoma:** FPS baixo, tempo alto em `nvnQueuePresentTexture` e uma interpretação do log como
milhares de barreiras completas e passes descartados. **Diagnóstico:** o contador antigo `barrier`
mede flags de comandos NVN; zero não emite `vk_full_barrier`. `rdiscard` mede rasterizer discard
usado no transform feedback. Esses contadores não provam o gargalo alegado. O código, porém,
usava barreiras globais inclusive para cópias e passes internos de entradas conhecidas, e dormia
com o lock global durante a apresentação de duas imagens interpoladas.

**Correção:** `exec.c` usa dependências de imagem nos consumidores conhecidos, preservando escopos
conservadores para guest/bindless/SSBO e readback compartilhado. `device.c` preserva a dependência
STORE/LOAD da janela e ordena o gate de apresentação antes do lock global; durante o pacing solta
só este último. Reapresentação e reinício usam a mesma ordem. `sync_profile.*` e o relatório separam
as esperas e contam os helpers de barreira realmente emitidos. `LARECOMP_FULL_BARRIERS=1` permite
comparar os novos escopos com barreiras globais.

**Validação:** regressões nativas sem assets cobrem progresso concorrente durante o sleep,
exclusividade do present, profundidade/stencil, camadas/mips, fallback global e retomada da janela;
testes do relatório distinguem capturas antigas e dados sobrepostos. As cinco unidades C alteradas
compilam com Clang via Zig e headers Vulkan/SDL3/VMA em Linux. Ganho de FPS e correção visual no
Windows/driver do usuário ainda dependem de execução no jogo; não há benchmark da nova versão.
