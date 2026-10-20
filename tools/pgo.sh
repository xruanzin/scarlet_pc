#!/bin/sh
# PGO do lasgame (codigo recompilado do jogo).  Uso:
#   tools/pgo.sh gen            # compila o lasgame instrumentado (build/pgo-gen)
#   (jogue 10-15 min: cidade/academia, campo aberto, batalha, loading; saia pelo menu)
#   tools/pgo.sh merge          # junta build/pgo-gen/prof/*.profraw em build/scarlet.profdata
#   tools/pgo.sh use            # compila o lasgame final com o perfil (build/pgo-use)
# Variaveis: MAKE_ARGS="-j12 GEN=generated DEPS=..." repassadas ao make (as mesmas do seu build normal).
set -eu
cd "$(dirname "$0")/.."
PROF="$(pwd)/build/scarlet.profdata"
case "${1:-}" in
gen)
    mkdir -p build/pgo-gen/prof
    make ${MAKE_ARGS:-} B=build/pgo-gen PGO=gen all
    echo "Rode com: LLVM_PROFILE_FILE=$(pwd)/build/pgo-gen/prof/scarlet-%p-%m.profraw build/pgo-gen/<exe>"
    ;;
merge)
    set -- build/pgo-gen/prof/*.profraw
    [ -e "$1" ] || { echo "nenhum .profraw em build/pgo-gen/prof" >&2; exit 1; }
    llvm-profdata merge -o "$PROF" "$@"
    echo "gerado $PROF"
    ;;
use)
    [ -f "$PROF" ] || { echo "falta $PROF (rode merge)" >&2; exit 1; }
    make ${MAKE_ARGS:-} B=build/pgo-use PGO=use PGO_PROFILE="$PROF" all
    ;;
*) echo "uso: $0 gen|merge|use" >&2; exit 2 ;;
esac
