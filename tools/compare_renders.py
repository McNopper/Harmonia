#!/usr/bin/env python3
"""
compare_renders.py — Hyperion / Theia pre-tonemap HDR diff tool.

USAGE
-----
    python tools/compare_renders.py <reference.exr> <candidate.exr> [options]

    reference  : Hyperion output  (--output ref.exr)
    candidate  : Theia output     (--output cand.exr)

OPTIONS
    --threshold FLOAT   Mean-diff threshold for pass/fail (default: 4.0, in 1/255 units)
    --psnr-threshold FLOAT
                        Pass if PSNR >= this value (dB)
    --relative-threshold FLOAT
                        Pass if relative mean error <= this value (percent)
    --gate MODE         Gate mode: mean | psnr | relative | any | all | scale-aware (default: mean)
                        scale-aware = pass if mean_diff <= threshold OR
                                      (rel_mean_% <= relative-threshold and PSNR >= psnr-threshold)
                                      with defaults 2.5% / 20 dB when not provided.
    --heatmap PATH      Write a false-color difference PNG (default: <candidate>_diff.png)
    --channel CH        Compare single channel: r, g, b, luminance (default: luminance)
    --no-heatmap        Skip heatmap output

CONTRACT
--------
Both EXR files must be:
  - Scene-referred linear (produced by --output *.exr, never *.png)
  - Same resolution
  - Same working color space (both renderers must use the same [render] preset)
  - Rendered without SSR / SSAO / bloom (Theia: pass --no-postfx; Hyperion has none)
  - Pre-tonemap: the EXR from harmonia::App::renderOffscreen() is the raw HDR buffer

METRICS
-------
  mean_diff   : mean |ref - cand| per luminance pixel, scaled to [0, 255]
  max_diff    : max  |ref - cand| per luminance pixel, scaled to [0, 255]
  p99_diff    : 99th percentile of |ref-cand| (scaled to [0,255])
  p99.9_diff  : 99.9th percentile of |ref-cand| (scaled to [0,255])
  PSNR        : peak signal-to-noise ratio (dB); inf = identical; < 30 dB = visible
  rel_mean_%  : mean |ref-cand| / max(mean |ref|, eps) * 100
  pass        : selected gate mode with the provided threshold(s)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np


# ── EXR I/O ──────────────────────────────────────────────────────────────────

def load_exr(path: Path) -> np.ndarray:
    """Load an OpenEXR file and return a float32 HxWx3 array (RGB)."""
    try:
        import OpenEXR
        import Imath
    except ImportError:
        sys.exit("ERROR: OpenEXR Python bindings not found.  Run: pip install openexr imath")

    f = OpenEXR.InputFile(str(path))
    header = f.header()
    dw = header["dataWindow"]
    width  = dw.max.x - dw.min.x + 1
    height = dw.max.y - dw.min.y + 1

    FLOAT = Imath.PixelType(Imath.PixelType.FLOAT)
    channels = {}
    for ch in ("R", "G", "B"):
        raw = f.channel(ch, FLOAT)
        channels[ch] = np.frombuffer(raw, dtype=np.float32).reshape(height, width)

    return np.stack([channels["R"], channels["G"], channels["B"]], axis=-1)


# ── Metrics ───────────────────────────────────────────────────────────────────

def luminance_rec2020(img: np.ndarray) -> np.ndarray:
    """Rec.2020 luminance coefficients (linear)."""
    return 0.2627 * img[..., 0] + 0.6780 * img[..., 1] + 0.0593 * img[..., 2]


def luminance_rec709(img: np.ndarray) -> np.ndarray:
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def extract_channel(img: np.ndarray, ch: str) -> np.ndarray:
    ch = ch.lower()
    if ch == "r":
        return img[..., 0]
    if ch == "g":
        return img[..., 1]
    if ch == "b":
        return img[..., 2]
    if ch in ("lum", "luminance", "y"):
        return luminance_rec2020(img)
    sys.exit(f"ERROR: unknown channel '{ch}'. Use r, g, b, or luminance.")


def compute_metrics(ref: np.ndarray, cand: np.ndarray) -> dict:
    diff = np.abs(ref - cand)
    # Scale to [0, 255] range via the "255" convention: treat 1.0 HDR = 255 units.
    # This makes thresholds intuitive (4.0 = 4/255 ≈ 1.6% of SDR white).
    diff255 = diff * 255.0
    mean_d = float(np.mean(diff255))
    max_d  = float(np.max(diff255))
    p99_d = float(np.percentile(diff255, 99.0))
    p999_d = float(np.percentile(diff255, 99.9))
    mse    = float(np.mean(diff ** 2))
    mean_ref = float(np.mean(np.abs(ref)))
    rel_mean_pct = float((np.mean(diff) / max(mean_ref, 1.0e-8)) * 100.0)
    # PSNR relative to peak=1.0 (HDR-scene-referred white)
    psnr   = float("inf") if mse == 0.0 else float(10.0 * np.log10(1.0 / mse))
    return {
        "mean_diff": mean_d,
        "max_diff": max_d,
        "p99_diff": p99_d,
        "p999_diff": p999_d,
        "psnr": psnr,
        "mse": mse,
        "rel_mean_pct": rel_mean_pct,
    }


def evaluate_gate(metrics: dict, args: argparse.Namespace) -> tuple[bool, list[str]]:
    checks = {
        "mean": metrics["mean_diff"] <= args.threshold,
        "psnr": (args.psnr_threshold is not None) and (metrics["psnr"] >= args.psnr_threshold),
        "relative": (args.relative_threshold is not None) and (metrics["rel_mean_pct"] <= args.relative_threshold),
    }

    if args.gate == "mean":
        return checks["mean"], [f"mean_diff <= {args.threshold}"]
    if args.gate == "psnr":
        if args.psnr_threshold is None:
            sys.exit("ERROR: --gate psnr requires --psnr-threshold")
        return checks["psnr"], [f"PSNR >= {args.psnr_threshold} dB"]
    if args.gate == "relative":
        if args.relative_threshold is None:
            sys.exit("ERROR: --gate relative requires --relative-threshold")
        return checks["relative"], [f"rel_mean_% <= {args.relative_threshold}%"]
    if args.gate == "any":
        available = [("mean", checks["mean"])]
        labels = [f"mean_diff <= {args.threshold}"]
        if args.psnr_threshold is not None:
            available.append(("psnr", checks["psnr"]))
            labels.append(f"PSNR >= {args.psnr_threshold} dB")
        if args.relative_threshold is not None:
            available.append(("relative", checks["relative"]))
            labels.append(f"rel_mean_% <= {args.relative_threshold}%")
        return any(v for _, v in available), labels
    if args.gate == "all":
        available = [checks["mean"]]
        labels = [f"mean_diff <= {args.threshold}"]
        if args.psnr_threshold is not None:
            available.append(checks["psnr"])
            labels.append(f"PSNR >= {args.psnr_threshold} dB")
        if args.relative_threshold is not None:
            available.append(checks["relative"])
            labels.append(f"rel_mean_% <= {args.relative_threshold}%")
        return all(available), labels
    if args.gate == "scale-aware":
        # HDR/transmissive scenes can violate a fixed absolute 1/255 threshold even
        # when the relative error is small; accept either strict absolute pass OR
        # relative+PSNR pass.
        psnr_threshold = args.psnr_threshold if args.psnr_threshold is not None else 20.0
        relative_threshold = args.relative_threshold if args.relative_threshold is not None else 2.5
        absolute_pass = checks["mean"]
        relative_psnr_pass = (metrics["rel_mean_pct"] <= relative_threshold) and (metrics["psnr"] >= psnr_threshold)
        labels = [
            f"mean_diff <= {args.threshold}",
            f"(rel_mean_% <= {relative_threshold}% and PSNR >= {psnr_threshold} dB)",
        ]
        return absolute_pass or relative_psnr_pass, labels

    sys.exit(f"ERROR: unknown gate mode '{args.gate}'")


# ── Heatmap ───────────────────────────────────────────────────────────────────

def save_heatmap(diff_channel: np.ndarray, out_path: Path) -> None:
    """Write a false-color heatmap PNG.  Blue=0, Green=small, Red=large diff."""
    try:
        import imageio.v3 as iio
    except ImportError:
        sys.exit("ERROR: imageio not found.  Run: pip install imageio")

    # Normalise diff to [0, 1] using 95th percentile to avoid outlier saturation.
    p95 = float(np.percentile(diff_channel, 95))
    if p95 < 1e-9:
        p95 = 1.0
    normed = np.clip(diff_channel / p95, 0.0, 1.0)

    # Jet-like colormap: blue → cyan → green → yellow → red
    r = np.clip(1.5 - abs(normed * 4 - 3),   0.0, 1.0)
    g = np.clip(1.5 - abs(normed * 4 - 2),   0.0, 1.0)
    b = np.clip(1.5 - abs(normed * 4 - 1),   0.0, 1.0)
    heatmap = (np.stack([r, g, b], axis=-1) * 255).astype(np.uint8)

    iio.imwrite(str(out_path), heatmap)
    print(f"  heatmap -> {out_path}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference",  type=Path, help="Hyperion EXR (ground truth)")
    parser.add_argument("candidate",  type=Path, help="Theia EXR (under test)")
    parser.add_argument("--threshold", type=float, default=4.0,
                        help="Pass/fail mean-diff threshold in 1/255 units (default: 4.0)")
    parser.add_argument("--psnr-threshold", type=float, default=None,
                        help="Pass/fail PSNR threshold in dB (pass when PSNR >= threshold)")
    parser.add_argument("--relative-threshold", type=float, default=None,
                        help="Pass/fail relative mean error threshold in percent")
    parser.add_argument("--gate", choices=("mean", "psnr", "relative", "any", "all", "scale-aware"), default="mean",
                        help="Gate mode for RESULT (default: mean)")
    parser.add_argument("--heatmap",  type=Path, default=None,
                        help="Output heatmap PNG path (default: <candidate>_diff.png)")
    parser.add_argument("--channel",  default="luminance",
                        help="Channel to compare: r, g, b, luminance (default: luminance)")
    parser.add_argument("--no-heatmap", action="store_true", help="Skip heatmap output")
    args = parser.parse_args()

    if not args.reference.exists():
        sys.exit(f"ERROR: reference file not found: {args.reference}")
    if not args.candidate.exists():
        sys.exit(f"ERROR: candidate file not found: {args.candidate}")

    print(f"Reference : {args.reference}")
    print(f"Candidate : {args.candidate}")
    print(f"Channel   : {args.channel}")

    ref  = load_exr(args.reference)
    cand = load_exr(args.candidate)

    if ref.shape != cand.shape:
        sys.exit(f"ERROR: shape mismatch — reference {ref.shape} vs candidate {cand.shape}")

    ref_ch  = extract_channel(ref,  args.channel)
    cand_ch = extract_channel(cand, args.channel)
    metrics = compute_metrics(ref_ch, cand_ch)

    passed, gate_labels = evaluate_gate(metrics, args)

    print()
    print(f"  mean_diff : {metrics['mean_diff']:7.3f}  (threshold {args.threshold:.1f})")
    print(f"  max_diff  : {metrics['max_diff']:7.3f}")
    print(f"  p99_diff  : {metrics['p99_diff']:7.3f}")
    print(f"  p99.9_diff: {metrics['p999_diff']:7.3f}")
    print(f"  PSNR      : {metrics['psnr']:7.2f} dB")
    print(f"  MSE       : {metrics['mse']:.6f}")
    print(f"  rel_mean% : {metrics['rel_mean_pct']:7.3f} %")
    print(f"  gate      : {args.gate} ({'; '.join(gate_labels)})")
    print()
    print(f"  RESULT    : {'PASS' if passed else 'FAIL'}")

    if not args.no_heatmap:
        heatmap_path = args.heatmap or args.candidate.with_name(
            args.candidate.stem + "_diff.png")
        diff_ch = np.abs(ref_ch - cand_ch)
        save_heatmap(diff_ch, heatmap_path)

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
