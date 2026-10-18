# Gerador ARM64 → C

Copiado do LASRecomp (`tools/`, licença MIT, © 2026 Peiikai): `gen.py`, `lifter.py`, `analyze.py`, `nso.py`,
`passes.py`, `simd_c.py` sem alterações. `patches.py` foi esvaziado (os patches eram endereços do Link's Awakening).

Uso (a partir desta pasta, com o venv do projeto):

    ../../.venv/Scripts/python gen.py main    ../../extracted/exefs/main    0x7000000000 ../../generated/main --simd c
    ../../.venv/Scripts/python gen.py sdk     ../../extracted/exefs/sdk     0x7006000000 ../../generated/sdk  --simd c
    ../../.venv/Scripts/python gen.py subsdk0 ../../extracted/exefs/subsdk0 0x7008000000 ../../generated/subsdk0 --simd c
