# PGO do codigo recompilado (lasgame)

Motivo: em cenas pesadas o quadro leva 52-125 ms e o backend Vulkan responde por 13-31 ms; o resto e CPU no
codigo ARM64->x64 recompilado e no HLE. O codigo ja usa `-O3 -march=x86-64-v3`; PGO e o proximo passo sem mudar logica.

## Como usar
1. `tools/pgo.sh gen` (compila instrumentado em `build/pgo-gen`).
2. Rode com `LLVM_PROFILE_FILE=<abs>/build/pgo-gen/prof/scarlet-%p-%m.profraw`, jogue 10-15 min cobrindo cidade/academia,
   campo aberto, batalha e loading, e feche pelo menu (o perfil e gravado ao sair). O build instrumentado e mais lento.
3. `tools/pgo.sh merge` e depois `tools/pgo.sh use`.

## Notas e riscos
- Requer clang com `llvm-profdata` da mesma versao; no Windows, o runtime de perfil do clang precisa estar disponivel.
- O perfil vale para o mesmo codigo gerado: se `tools/gen.py` ou a versao do jogo mudar, regrave (funcoes sem match so geram aviso).
- Nao medido ainda: compare `[vk] fps` e `[hitch] ... other` do mesmo trajeto antes e depois. Se nao houver ganho, nao use.
- So cobre o `make`; o build pela tela inicial (`runtime/gamebuild.c`) nao recebe PGO.
