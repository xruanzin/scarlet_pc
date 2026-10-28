#!/usr/bin/env python3
"""Compare synchronized suyu and native Scarlet screenshots without rescaling.

Pillow required: python -m pip install pillow
Game assets remain local. No screenshots are uploaded or written to the repo.
"""
import argparse
import math
from pathlib import Path


def compare(reference: Path, candidate: Path, diff_path: Path) -> int:
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError:
        raise SystemExit("Install Pillow locally: python -m pip install pillow")

    with Image.open(reference) as a_file, Image.open(candidate) as b_file:
        a = a_file.convert("RGB")
        b = b_file.convert("RGB")
    if a.size != b.size:
        raise SystemExit(
            f"Image sizes differ: suyu={a.size}, native={b.size}. "
            "Capture both at the same resolution; no automatic resizing."
        )
    if a.size[0] <= 0 or a.size[1] <= 0:
        raise SystemExit("Invalid screenshot dimensions.")
    diff = ImageChops.difference(a, b)
    stat = ImageStat.Stat(diff)
    mean = stat.mean
    rms = stat.rms
    n = a.size[0] * a.size[1]
    mae = sum(mean) / 3
    mse = sum(v * v for v in rms) / 3
    psnr = math.inf if mse == 0 else 10 * math.log10(255 * 255 / mse)
    per_channel_hist = diff.histogram()
    # Channel-wide 95th percentile, independent of image size and without
    # allocating a several-million-element sorted pixel list.
    per_channel_p95 = []
    for channel in range(3):
        hist = per_channel_hist[channel * 256:(channel + 1) * 256]
        cutoff = math.ceil(n * 0.95)
        seen = 0
        for value, count in enumerate(hist):
            seen += count
            if seen >= cutoff:
                per_channel_p95.append(value)
                break
    diff_path.parent.mkdir(parents=True, exist_ok=True)
    # Amplified difference visualization; never alter the source screenshots.
    diff.point(lambda value: min(255, value * 4)).save(diff_path)
    print(f"Resolution: {a.width}x{a.height}")
    print(f"MAE RGB: R={mean[0]:.3f}, G={mean[1]:.3f}, B={mean[2]:.3f} (0 means identical)")
    print(f"MAE overall: {mae:.3f}/255; PSNR: {psnr:.2f} dB")
    print(f"RGB p95 absolute error: {per_channel_p95}")
    print(f"Amplified difference: {diff_path}")
    print("Interpret only synchronized same-scene/same-resolution screenshots.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suyu", required=True, type=Path, help="Unaltered reference screenshot from suyu")
    parser.add_argument("--native", required=True, type=Path, help="Native Scarlet screenshot of the exact same scene")
    parser.add_argument("--diff", required=True, type=Path, help="PNG path for amplified difference heatmap")
    args = parser.parse_args()
    return compare(args.suyu, args.native, args.diff)


if __name__ == "__main__":
    raise SystemExit(main())
