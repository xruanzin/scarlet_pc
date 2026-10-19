#!/usr/bin/env python3
"""Find the block-linear block height of textures dumped with LARECOMP_DUMP_TEX_RAW.

The runtime writes the still-swizzled level 0 of selected textures to build/texraw/<addr>_<w>x<h>_f<fmt>_bh<N>.bin
(index.txt lists the layout it used). This tool de-swizzles each file with every block height (0..5), decodes the
BC1/BC3/BC4/BC5/BC7 blocks with Pillow and scores how smooth the result is: a correct layout gives a photo-like image
(neighbouring pixels are similar), a wrong one gives noise. It prints the score per candidate and writes a contact sheet
per texture, so a wrong choice in runtime/nvn/objects.c can be seen without running the game.

    python tools/texraw_probe.py build/texraw            # every dump
    python tools/texraw_probe.py build/texraw --selftest # checks the de-swizzle against a synthetic texture
"""
import argparse
import os
import re
import struct
import sys

import numpy as np
from PIL import Image

# NVN format -> (bytes per 4x4 block, Pillow bcn index, Pillow mode)
BC = {
    0x41: (8, 1, "RGBA"), 0x42: (8, 1, "RGBA"), 0x45: (8, 1, "RGBA"), 0x46: (8, 1, "RGBA"),
    0x43: (16, 2, "RGBA"), 0x47: (16, 2, "RGBA"),
    0x44: (16, 3, "RGBA"), 0x48: (16, 3, "RGBA"),
    0x49: (8, 4, "L"), 0x4A: (8, 4, "L"),
    0x4B: (16, 5, "RGB"), 0x4C: (16, 5, "RGB"),
    0x4D: (16, 7, "RGBA"), 0x4E: (16, 7, "RGBA"),
}


def gob_offsets(wb_bytes, hb, bh_log2):
    """Byte offset in the block-linear surface of every 16-byte run of every row: array [hb, runs]."""
    runs = (wb_bytes + 15) // 16
    x = (np.arange(runs, dtype=np.uint64) * 16)[None, :]
    y = np.arange(hb, dtype=np.uint64)[:, None]
    width_gobs = (wb_bytes + 63) // 64
    gob_x, gob_y = x >> 6, y >> 3
    block_y = gob_y >> bh_log2
    gob_in_block = gob_y & ((1 << bh_log2) - 1)
    block_size = 512 << bh_log2
    off = block_y * width_gobs * block_size + gob_x * block_size + gob_in_block * 512
    xi, yi = x & 63, y & 7
    off = off + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4))
    return off.astype(np.int64)


def effective_bh(bh_log2, hb):
    """The runtime's rule (tegra.c level_block_height): a block never exceeds the level height."""
    while bh_log2 > 0 and (8 << (bh_log2 - 1)) >= hb:
        bh_log2 -= 1
    return bh_log2


def deswizzle(raw, wb, hb, bpb, bh_log2):
    bh_log2 = effective_bh(bh_log2, hb)
    row = wb * bpb
    off = gob_offsets(row, hb, bh_log2)
    padded = row + (-row) % 16
    need = int(off.max()) + 16
    buf = np.frombuffer(raw, dtype=np.uint8)
    if buf.size < need:
        raise ValueError("dump too small for this block height")
    out = np.zeros((hb, padded), dtype=np.uint8)
    for i in range(16):
        out[:, i::16] = buf[off + i]
    return out[:, :row].tobytes()


def swizzle(lin, wb, hb, bpb, bh_log2):
    """Inverse of deswizzle (used by --selftest)."""
    bh_log2 = effective_bh(bh_log2, hb)
    row = wb * bpb
    off = gob_offsets(row, hb, bh_log2)
    size = int(off.max()) + 16
    out = np.zeros(size, dtype=np.uint8)
    padded = row + (-row) % 16
    src = np.zeros((hb, padded), dtype=np.uint8)
    src[:, :row] = np.frombuffer(lin, dtype=np.uint8).reshape(hb, row)
    for i in range(16):
        out[off + i] = src[:, i::16]
    return out.tobytes()


def decode(blocks, w, h, fmt):
    bpb, n, mode = BC[fmt]
    wb, hb = (w + 3) // 4, (h + 3) // 4
    im = Image.frombuffer(mode, (wb * 4, hb * 4), blocks, "bcn", (n,))
    return im.convert("RGB")


def smoothness(im):
    a = np.asarray(im.convert("L"), dtype=np.float32)
    dx = np.abs(np.diff(a, axis=1)).mean()
    dy = np.abs(np.diff(a, axis=0)).mean()
    return float((dx + dy) / 2)


def probe(path, w, h, fmt, out_dir):
    bpb = BC[fmt][0]
    wb, hb = (w + 3) // 4, (h + 3) // 4
    raw = open(path, "rb").read()
    results = []
    thumbs = []
    seen = set()
    for bh in range(6):
        if effective_bh(bh, hb) in seen:
            continue
        seen.add(effective_bh(bh, hb))
        try:
            blocks = deswizzle(raw, wb, hb, bpb, bh)
            im = decode(blocks, w, h, fmt)
        except Exception as e:  # noqa: BLE001
            results.append((bh, None))
            continue
        results.append((bh, smoothness(im)))
        t = im.copy()
        t.thumbnail((200, 200))
        thumbs.append((bh, t))
    ok = [r for r in results if r[1] is not None]
    best = min(ok, key=lambda r: r[1]) if ok else None
    sheet = Image.new("RGB", (6 * 205, 205), (30, 0, 30))
    for k, (bh, t) in enumerate(thumbs):
        sheet.paste(t, (k * 205, 0))
    sheet.save(os.path.join(out_dir, os.path.basename(path)[:-4] + "_candidates.png"))
    return results, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?", default="build/texraw")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    pat = re.compile(r"([0-9a-f]+)_(\d+)x(\d+)_f([0-9a-f]+)_bh(\d+)\.bin$")
    files = sorted(f for f in os.listdir(a.dir) if pat.match(f))
    if not files:
        print("no dumps in", a.dir)
        return 1
    out_dir = os.path.join(a.dir, "probe")
    os.makedirs(out_dir, exist_ok=True)
    for f in files:
        addr, w, h, fmt, used = pat.match(f).groups()
        w, h, fmt, used = int(w), int(h), int(fmt, 16), int(used)
        if fmt not in BC:
            print(f"{f}: format {fmt:#x} not a BCn format handled here")
            continue
        results, best = probe(os.path.join(a.dir, f), w, h, fmt, out_dir)
        line = "  ".join(f"bh{bh}={'n/a' if s is None else f'{s:.1f}'}" for bh, s in results)
        verdict = "-"
        if best:
            verdict = "runtime choice OK" if effective_bh(best[0], (h + 3) // 4) == effective_bh(used, (h + 3) // 4) else f"BEST bh={best[0]} (runtime used {used})"
        print(f"{f}: {line}  -> {verdict}")
    print("contact sheets:", out_dir)
    return 0


def selftest():
    w = h = 512
    wb = hb = 128
    blocks = bytearray()
    for by in range(hb):
        for bx in range(wb):
            v = (bx * 2 + by * 2 + (bx * by) % 5) & 0xFF
            blocks += bytes([v, v]) + bytes(6)
    lin = bytes(blocks)
    ok = True
    for true_bh in (0, 1, 2, 3, 4):
        raw = swizzle(lin, wb, hb, 8, true_bh)
        assert deswizzle(raw, wb, hb, 8, true_bh) == lin, "round trip failed"
        # the runtime dumps the size of the largest candidate; mimic it so every height can be tried
        big = raw + bytes(len(raw) * 2)
        scores = {}
        for bh in range(6):
            eff = effective_bh(bh, hb)
            if eff in scores:
                continue
            im = decode(deswizzle(big, wb, hb, 8, bh), w, h, 0x49)
            scores[eff] = smoothness(im)
        best = min(scores, key=scores.get)
        print(f"true bh={true_bh}: best={best} scores={ {k: round(v, 1) for k, v in scores.items()} }")
        ok = ok and best == effective_bh(true_bh, hb)
    print("selftest", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
