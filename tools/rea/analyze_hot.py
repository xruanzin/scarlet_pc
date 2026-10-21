#!/usr/bin/env python3
import argparse, json, pathlib, re, sys

MODULES = [
    ("main", 0x7000000000, 0x7006000000),
    ("sdk", 0x7006000000, 0x7008000000),
    ("subsdk0", 0x7008000000, 0x7800000000),
]

def parse_int(s):
    s = s.strip()
    return int(s, 16) if re.fullmatch(r"[0-9A-Fa-f]+", s) else int(s, 0)

def classify(count):
    if count >= 500_000: return "critical"
    if count >= 100_000: return "very_high"
    if count >= 25_000: return "high"
    if count >= 5_000: return "medium"
    return "low"

def module_for(addr):
    for name, lo, hi in MODULES:
        if lo <= addr < hi:
            return name, addr - lo
    return "unknown", addr

def iter_rows(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                addr = parse_int(parts[0])
                count = int(parts[1], 0)
                first = int(parts[2], 0) if len(parts) > 2 else 0
                lr = parse_int(parts[3]) if len(parts) > 3 else 0
            except ValueError:
                continue
            yield addr, count, first, lr

def main():
    ap = argparse.ArgumentParser(description="Merge Scarlet fnprof captures and rank hot guest functions.")
    ap.add_argument("profiles", nargs="+", help="fnprof*.txt files")
    ap.add_argument("-o", "--out", default="analysis/rea/hot_functions.json")
    args = ap.parse_args()

    files = []
    for pattern in args.profiles:
        p = pathlib.Path(pattern)
        if any(ch in pattern for ch in "*?[]"):
            files.extend(pathlib.Path().glob(pattern))
        elif p.exists():
            files.append(p)

    uniq = []
    seen = set()
    for p in files:
        rp = p.resolve()
        if rp not in seen:
            seen.add(rp)
            uniq.append(p)

    if not uniq:
        print("[rea-hot] nenhum fnprof encontrado", file=sys.stderr)
        return 2

    merged = {}
    for p in uniq:
        for addr, count, first, lr in iter_rows(p):
            item = merged.setdefault(addr, {"count":0, "first":first, "lr":lr, "sources":[]})
            item["count"] += count
            if not item["first"] or (first and first < item["first"]):
                item["first"] = first
                item["lr"] = lr
            item["sources"].append(p.name)

    rows = []
    for addr, item in merged.items():
        mod, off = module_for(addr)
        rows.append({
            "address": f"0x{addr:x}",
            "address_int": addr,
            "module": mod,
            "module_offset": f"0x{off:x}",
            "count": item["count"],
            "severity": classify(item["count"]),
            "first": item["first"],
            "first_lr": f"0x{item['lr']:x}",
            "sources": sorted(set(item["sources"])),
        })
    rows.sort(key=lambda x: x["count"], reverse=True)

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({
        "profiles": [str(p) for p in uniq],
        "function_count": len(rows),
        "functions": rows,
    }, indent=2), encoding="utf-8")

    print(f"[rea-hot] {len(rows)} funcoes -> {out}")
    for i, r in enumerate(rows[:25], 1):
        print(f"{i:2d}. {r['count']:>10}  {r['address']}  {r['module']}+{r['module_offset']}  {r['severity']}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
