#!/usr/bin/env python3
"""Fast ARM64 lifter compatibility probe for Pokémon Scarlet ExeFS.

Scans every 4-byte instruction in the text segments of main/sdk/subsdk0 and asks
the existing LAS/Let's Go lifter to decode it. It does not generate C and does
not modify game files.

Usage:
    python tools/scarlet_lift_probe.py D:/PokemonScarlet/exefs --json scarlet_lift.json
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

from lifter import Lifter, Unsupported
from nso import Nso

MODULES = (
    ("main", 0x7000000000),
    ("sdk", 0x7006000000),
    ("subsdk0", 0x7008000000),
)


def scan(path: Path, name: str, base: int) -> dict:
    n = Nso(str(path))
    text_off, text_size = n.text
    text = bytes(n.image[text_off:text_off + text_size])
    words = struct.unpack(f"<{len(text) // 4}I", text[: len(text) & ~3])
    lifter = Lifter(base, "c", False)

    kinds = Counter()
    unsupported = Counter()
    samples = defaultdict(list)

    for i, word in enumerate(words):
        addr = base + text_off + i * 4
        try:
            ins = lifter.lift(word, addr)
            kinds[ins.kind] += 1
        except Unsupported as exc:
            reason = str(exc)
            unsupported[reason] += 1
            if len(samples[reason]) < 8:
                samples[reason].append({"address": f"0x{addr:x}", "word": f"0x{word:08x}"})

    return {
        "name": name,
        "build_id": n.build_id,
        "base": f"0x{base:x}",
        "text_size": text_size,
        "instructions": len(words),
        "kinds": dict(sorted(kinds.items())),
        "unsupported_total": sum(unsupported.values()),
        "unsupported": [
            {"reason": reason, "count": count, "samples": samples[reason]}
            for reason, count in unsupported.most_common()
        ],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Probe Scarlet ARM64 instructions against the current static-recomp lifter.")
    ap.add_argument("exefs", help="extracted ExeFS folder containing main, sdk and subsdk0")
    ap.add_argument("--json", dest="json_path", help="write the full report as JSON")
    args = ap.parse_args()

    exefs = Path(args.exefs)
    reports = []
    missing = []
    for name, base in MODULES:
        path = exefs / name
        if not path.is_file():
            missing.append(name)
            continue
        print(f"[probe] {name}: scanning {path} ...", file=sys.stderr, flush=True)
        r = scan(path, name, base)
        reports.append(r)
        print(
            f"{name}: {r['instructions']} instructions, "
            f"{r['unsupported_total']} unsupported, build {r['build_id'][:16]}"
        )
        for u in r["unsupported"][:12]:
            sample = ", ".join(x["address"] for x in u["samples"][:3])
            print(f"  {u['count']:8d}  {u['reason']}  [{sample}]")

    result = {
        "schema": 1,
        "exefs": str(exefs),
        "modules": reports,
        "unsupported_total": sum(r["unsupported_total"] for r in reports),
        "missing_modules": missing,
    }

    if args.json_path:
        out = Path(args.json_path)
        out.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(f"JSON report: {out}")

    if missing:
        print("Missing modules: " + ", ".join(missing), file=sys.stderr)
        return 2
    return 0 if result["unsupported_total"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
