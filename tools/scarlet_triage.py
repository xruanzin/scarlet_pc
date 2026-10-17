#!/usr/bin/env python3
"""Pokémon Scarlet static-recompilation triage.

This tool never modifies or copies game files. It inspects one or more extracted
ExeFS folders (or parent folders containing ExeFS) and reports the executable
modules that a Scarlet recompilation runtime will need to load.

Usage:
    python tools/scarlet_triage.py D:/Scarlet/base/exefs
    python tools/scarlet_triage.py D:/Scarlet/base D:/Scarlet/update --json scarlet_triage.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from collections import Counter
from pathlib import Path
from typing import Any

from nso import Nso

MAX_SCAN_DEPTH = 6


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def npdm_program_id(npdm: Path) -> int | None:
    try:
        raw = npdm.read_bytes()
    except OSError:
        return None
    if len(raw) < 0x80 or raw[:4] != b"META":
        return None
    aci_off = struct.unpack_from("<I", raw, 0x70)[0]
    if aci_off + 0x18 > len(raw) or raw[aci_off:aci_off + 4] != b"ACI0":
        return None
    return struct.unpack_from("<Q", raw, aci_off + 0x10)[0]


def is_exefs(path: Path) -> bool:
    return path.is_dir() and (path / "main").is_file() and (path / "main.npdm").is_file()


def find_exefs(root: Path) -> Path | None:
    root = root.resolve()
    if is_exefs(root):
        return root
    direct = root / "exefs"
    if is_exefs(direct):
        return direct

    base_depth = len(root.parts)
    try:
        for current, dirs, files in os.walk(root):
            p = Path(current)
            depth = len(p.parts) - base_depth
            if depth > MAX_SCAN_DEPTH:
                dirs[:] = []
                continue
            dirs[:] = [d for d in dirs if d.lower() not in {"romfs", "cache", "generated", "build"}]
            if "main" in files and "main.npdm" in files:
                return p
    except OSError:
        pass
    return None


def nso_paths(exefs: Path) -> list[Path]:
    out: list[Path] = []
    for p in sorted(exefs.iterdir(), key=lambda x: x.name.lower()):
        if not p.is_file() or p.name == "main.npdm":
            continue
        try:
            with p.open("rb") as f:
                if f.read(4) == b"NSO0":
                    out.append(p)
        except OSError:
            continue
    return out


def analyze_module(path: Path) -> dict[str, Any]:
    item: dict[str, Any] = {
        "name": path.name,
        "file_size": path.stat().st_size,
        "sha256": sha256_file(path),
    }
    try:
        n = Nso(str(path))
        relocs = Counter(r[1] for r in n.relocs)
        item.update({
            "build_id": n.build_id,
            "image_size": n.size,
            "text": {"offset": n.text[0], "size": n.text[1]},
            "rodata": {"offset": n.rodata[0], "size": n.rodata[1]},
            "data": {"offset": n.data[0], "size": n.data[1]},
            "bss_size": n.bss_size,
            "needed": list(n.needed),
            "symbols": len(n.syms),
            "imports": len(n.imports()),
            "exports": len(n.exports()),
            "relocations": {str(k): v for k, v in sorted(relocs.items())},
        })
    except Exception as exc:
        item["error"] = f"{type(exc).__name__}: {exc}"
    return item


def analyze_root(input_path: str) -> dict[str, Any]:
    root = Path(input_path).expanduser()
    exefs = find_exefs(root)
    result: dict[str, Any] = {
        "input": str(root),
        "exefs": str(exefs) if exefs else None,
        "program_id": None,
        "modules": [],
    }
    if exefs is None:
        result["error"] = "No extracted ExeFS containing main + main.npdm was found."
        return result

    pid = npdm_program_id(exefs / "main.npdm")
    result["program_id"] = f"{pid:016X}" if pid is not None else None
    result["modules"] = [analyze_module(p) for p in nso_paths(exefs)]
    return result


def human_size(n: int | None) -> str:
    if n is None:
        return "-"
    value = float(n)
    units = ["B", "KiB", "MiB", "GiB"]
    unit = units[0]
    for unit in units:
        if value < 1024.0 or unit == units[-1]:
            break
        value /= 1024.0
    return f"{value:.2f} {unit}"


def print_report(result: dict[str, Any], index: int) -> None:
    print("=" * 78)
    print(f"Pokémon Scarlet Recomp - ExeFS triage #{index}")
    print("=" * 78)
    print(f"Input      : {result['input']}")
    if result.get("error"):
        print(f"ERROR      : {result['error']}")
        return
    print(f"ExeFS      : {result['exefs']}")
    print(f"Program ID : {result.get('program_id') or 'unreadable'}")
    print(f"NSO modules: {len(result['modules'])}")
    print()

    for m in result["modules"]:
        print(f"[{m['name']}]")
        print(f"  file       {human_size(m.get('file_size'))}")
        print(f"  sha256     {m.get('sha256', '-')}")
        if m.get("error"):
            print(f"  parser     ERROR: {m['error']}")
            print()
            continue
        print(f"  build id   {m['build_id']}")
        print(f"  image      {human_size(m['image_size'])}")
        print(
            "  segments   "
            f"text={human_size(m['text']['size'])}  "
            f"rodata={human_size(m['rodata']['size'])}  "
            f"data={human_size(m['data']['size'])}  "
            f"bss={human_size(m['bss_size'])}"
        )
        print(f"  symbols    total={m['symbols']} imports={m['imports']} exports={m['exports']}")
        print(f"  needed     {', '.join(m['needed']) if m['needed'] else '-'}")
        print(f"  relocs     {m['relocations']}")
        print()


def compare(results: list[dict[str, Any]]) -> None:
    if len(results) < 2:
        return
    print("=" * 78)
    print("Module/build comparison")
    print("=" * 78)
    names = sorted({m["name"] for r in results for m in r.get("modules", [])})
    by_result = [{m["name"]: m for m in r.get("modules", [])} for r in results]
    for name in names:
        ids = []
        for mods in by_result:
            m = mods.get(name)
            ids.append((m or {}).get("build_id", "-")[:16] if m else "-")
        changed = len(set(ids)) > 1
        print(f"{'*' if changed else ' '} {name:<16} " + "  ".join(ids))
    print("* = differs between the supplied ExeFS sets")


def main() -> int:
    ap = argparse.ArgumentParser(description="Inspect extracted Switch ExeFS modules for the Scarlet recompilation port.")
    ap.add_argument("paths", nargs="+", help="ExeFS folder(s), or parent folders containing exefs/main + main.npdm")
    ap.add_argument("--json", dest="json_path", help="also write the complete metadata report as JSON")
    args = ap.parse_args()

    results = [analyze_root(p) for p in args.paths]
    for i, result in enumerate(results, 1):
        print_report(result, i)
    compare(results)

    if args.json_path:
        out = Path(args.json_path)
        out.write_text(json.dumps({"schema": 1, "sets": results}, indent=2), encoding="utf-8")
        print()
        print(f"JSON report: {out}")

    return 1 if any(r.get("error") for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
