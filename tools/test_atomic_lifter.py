#!/usr/bin/env python3
"""Regression checks for AArch64 exclusive-pair / CAS lifting."""

from lifter import Lifter

CASES = [
    # Scarlet samples from the base-game main.
    (0xC87F2788, "ldxp_pair64"),   # ldxp  x8, x9, [x28]
    (0xC82A2788, "stxp_pair64"),   # stxp  w10, x8, x9, [x28]
    (0xC87FAB8B, "ldxp_pair64"),   # ldaxp x11, x10, [x28]
    (0xC82DB388, "stxp_pair64"),   # stlxp w13, x8, x12, [x28]

    # LSE compare-and-swap coverage.
    (0xC8A07C82, "cas64"),         # cas   x0, x2, [x4]
    (0xC8E07C82, "cas64"),         # casa  x0, x2, [x4]
    (0xC8A0FC82, "cas64"),         # casl  x0, x2, [x4]
    (0xC8E0FC82, "cas64"),         # casal x0, x2, [x4]
    (0x48207C82, "casp_pair64"),   # casp  x0, x1, x2, x3, [x4]
    (0x08207C82, "casp_pair32"),   # casp  w0, w1, w2, w3, [x4]
]

l = Lifter(0x7000000000, "c", False)
for i, (word, want) in enumerate(CASES):
    ins = l.lift(word, 0x7000000000 + i * 4)
    assert ins.kind == "op", (hex(word), ins.kind)
    assert want in ins.code, (hex(word), want, ins.code)

print(f"atomic lifter: {len(CASES)} cases OK")
