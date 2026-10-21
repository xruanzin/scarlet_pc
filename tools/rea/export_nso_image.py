#!/usr/bin/env python3
"""Export Scarlet NSO images + hot-function seeds for REA/Ghidra raw ARM64 analysis."""
import argparse, json, pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(TOOLS))
from nso import Nso

BASES = {
    "main": 0x7000000000,
    "sdk": 0x7006000000,
    "subsdk0": 0x7008000000,
}

def parse_addr(v):
    return int(v, 0) if isinstance(v, str) else int(v)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exefs", required=True, help="directory containing main/sdk/subsdk0 NSO files")
    ap.add_argument("--hot-json", required=True, help="analysis/rea/hot_functions.json")
    ap.add_argument("--outdir", default="analysis/rea/raw")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    exefs = pathlib.Path(args.exefs)
    hot_path = pathlib.Path(args.hot_json)
    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    hot = json.loads(hot_path.read_text(encoding="utf-8"))
    funcs = hot.get("functions", [])[:max(1, args.top)]
    by_module = {name: [] for name in BASES}
    for item in funcs:
        mod = item.get("module")
        if mod in by_module:
            by_module[mod].append(item)

    manifest = {"hot_source": str(hot_path), "top": args.top, "modules": {}}
    for mod, base in BASES.items():
        src = exefs / mod
        if not src.is_file():
            continue
        nso = Nso(str(src))
        image_path = outdir / f"{mod}.image.bin"
        image_path.write_bytes(bytes(nso.image))

        seeds_path = outdir / f"{mod}.seeds.tsv"
        with seeds_path.open("w", encoding="utf-8", newline="\n") as f:
            f.write("# address\tkind\tname\n")
            for item in by_module[mod]:
                addr = parse_addr(item["address"])
                f.write(f"0x{addr:x}\tfunction\thot_{addr:x}\n")

        manifest["modules"][mod] = {
            "base": f"0x{base:x}",
            "build_id": nso.build_id,
            "image": str(image_path),
            "seeds": str(seeds_path),
            "image_size": len(nso.image),
            "text": {"offset": nso.text[0], "size": nso.text[1]},
            "hot_functions": by_module[mod],
        }
        print(f"[rea-export] {mod}: {image_path} ({len(nso.image)/1048576:.1f} MiB), {len(by_module[mod])} seeds")

    manifest_path = outdir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"[rea-export] manifest -> {manifest_path}")

if __name__ == "__main__":
    main()
